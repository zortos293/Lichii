// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cinttypes>
#include <memory>

#include "common/cpu_features.h"
#include "common/signal_chain.h"
#include "core/arm/nce/arm_nce.h"
#include "core/arm/nce/instructions.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/arm/nce/patcher.h"
#include "core/core.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Core {

namespace {

struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;
struct sigaction g_orig_ill_action;

// Switch 2 games target the Cortex-A78C, which has pointer authentication (ARMv8.3 PAuth).
// Hosts without it raise SIGILL on the non-hint PAuth instructions. With no PAC keys, signing
// and authenticating leave a pointer unchanged, so they are executed here as their plain forms.
// Returns false when the instruction is not one of them.
bool EmulatePointerAuthInstruction(u32 inst, mcontext_t& host_ctx, Core::Memory::Memory& memory) {
    const auto reg = [&](u32 index) -> u64 { return index == 31 ? 0 : host_ctx.regs[index]; };
    const auto set_reg = [&](u32 index, u64 value) {
        if (index != 31) {
            host_ctx.regs[index] = value;
        }
    };
    const u32 rd = inst & 0x1F;
    const u32 rn = (inst >> 5) & 0x1F;

    // PACIA, PACIB, PACDA, PACDB, AUTIA, AUTIB, AUTDA, AUTDB and their Z forms.
    if ((inst & 0xFFFFC000) == 0xDAC10000) {
        host_ctx.pc += 4;
        return true;
    }
    // XPACI, XPACD.
    if ((inst & 0xFFFFFBE0) == 0xDAC143E0) {
        host_ctx.pc += 4;
        return true;
    }
    // PACGA: only the upper 32 bits hold the code.
    if ((inst & 0xFFE0FC00) == 0x9AC03000) {
        set_reg(rd, 0);
        host_ctx.pc += 4;
        return true;
    }
    // RETAA, RETAB.
    if ((inst & 0xFFFFFBFF) == 0xD65F0BFF) {
        host_ctx.pc = host_ctx.regs[30];
        return true;
    }
    // BRAA, BRAB, BLRAA, BLRAB and their Z forms.
    if ((inst & 0xFEDFF800) == 0xD61F0800) {
        const u64 target = reg(rn);
        if ((inst >> 21) & 1) {
            host_ctx.regs[30] = host_ctx.pc + 4;
        }
        host_ctx.pc = target;
        return true;
    }
    // LDRAA, LDRAB: a 10-bit signed offset scaled by 8, with optional pre-index writeback.
    if ((inst & 0xFF200400) == 0xF8200400) {
        const u32 imm10 = (((inst >> 22) & 1) << 9) | ((inst >> 12) & 0x1FF);
        const s64 offset = static_cast<s64>(static_cast<s32>(imm10 << 22) >> 22) * 8;
        const u64 base = rn == 31 ? host_ctx.sp : host_ctx.regs[rn];
        const u64 address = base + offset;
        set_reg(rd, memory.Read64(address));
        if ((inst >> 11) & 1) {
            if (rn == 31) {
                host_ctx.sp = address;
            } else {
                host_ctx.regs[rn] = address;
            }
        }
        host_ctx.pc += 4;
        return true;
    }
    return false;
}

// Verify assembly offsets.
using NativeExecutionParameters = Kernel::KThread::NativeExecutionParameters;
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);
static_assert(offsetof(NativeExecutionParameters, magic) == TpidrEl0TlsMagic);

fpsimd_context* GetFloatingPointState(mcontext_t& host_ctx) {
    _aarch64_ctx* header = reinterpret_cast<_aarch64_ctx*>(&host_ctx.__reserved);
    while (header->magic != FPSIMD_MAGIC) {
        header = reinterpret_cast<_aarch64_ctx*>(reinterpret_cast<char*>(header) + header->size);
    }
    return reinterpret_cast<fpsimd_context*>(header);
}

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;

} // namespace

void* ArmNce::RestoreGuestContext(void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Thread-local parameters will be located in x9.
    auto* tpidr = reinterpret_cast<NativeExecutionParameters*>(host_ctx.regs[9]);
    auto* guest_ctx = static_cast<GuestContext*>(tpidr->native_context);

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save host callee-saved registers.
    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &fpctx->vregs[8],
                sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs[19],
                sizeof(guest_ctx->host_ctx.host_saved_regs));

    // Save stack pointer.
    guest_ctx->host_ctx.host_sp = host_ctx.sp;

    // Restore all guest state except tpidr_el0.
    host_ctx.sp = guest_ctx->sp;
    host_ctx.pc = guest_ctx->pc;
    host_ctx.pstate = guest_ctx->pstate;
    fpctx->fpcr = guest_ctx->fpcr;
    fpctx->fpsr = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs, guest_ctx->cpu_registers.data(), sizeof(host_ctx.regs));
    std::memcpy(fpctx->vregs, guest_ctx->vector_registers.data(), sizeof(fpctx->vregs));

    // Return the new thread-local storage pointer.
    return tpidr;
}

void ArmNce::SaveGuestContext(GuestContext* guest_ctx, void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);

    // Save all guest registers except tpidr_el0.
    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs, sizeof(host_ctx.regs));
    std::memcpy(guest_ctx->vector_registers.data(), fpctx->vregs, sizeof(fpctx->vregs));
    guest_ctx->fpsr = fpctx->fpsr;
    guest_ctx->fpcr = fpctx->fpcr;
    guest_ctx->pstate = static_cast<u32>(host_ctx.pstate);
    guest_ctx->pc = host_ctx.pc;
    guest_ctx->sp = host_ctx.sp;

    // Restore stack pointer.
    host_ctx.sp = guest_ctx->host_ctx.host_sp;

    // Restore host callee-saved registers.
    std::memcpy(&host_ctx.regs[19], guest_ctx->host_ctx.host_saved_regs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&fpctx->vregs[8], guest_ctx->host_ctx.host_saved_vregs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_vregs));

    // Return from the call on exit by setting pc to x30.
    host_ctx.pc = guest_ctx->host_ctx.host_saved_regs[11];

    // Clear esr_el1 and return it.
    host_ctx.regs[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleFailedGuestFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* info = static_cast<siginfo_t*>(raw_info);

    // We can't handle the access, so determine why we crashed.
    const bool is_prefetch_abort = host_ctx.pc == reinterpret_cast<u64>(info->si_addr);

    // For data aborts, skip the instruction and return to guest code.
    // This will allow games to continue in many scenarios where they would otherwise crash.
    if (!is_prefetch_abort) {
        host_ctx.pc += 4;
        return true;
    }

    // This is a prefetch abort.
    guest_ctx->esr_el1.fetch_or(static_cast<u64>(HaltReason::PrefetchAbort));

    // Forcibly mark the context as locked. We are still running.
    // We may race with SignalInterrupt here:
    // - If we lose the race, then SignalInterrupt will send us a signal we are masking,
    //   and it will do nothing when it is unmasked, as we have already left guest code.
    // - If we win the race, then SignalInterrupt will wait for us to unlock first.
    auto& thread_params = guest_ctx->parent->m_running_thread->GetNativeExecutionParameters();
    thread_params.lock.store(SpinLockLocked);

    // Return to host.
    SaveGuestContext(guest_ctx, raw_context);
    return false;
}

bool ArmNce::HandleGuestAlignmentFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* fpctx = GetFloatingPointState(host_ctx);
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();

    // Match and execute an instruction.
    auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx);
    if (next_pc) {
        host_ctx.pc = *next_pc;
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

bool ArmNce::HandleGuestAccessFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto* info = static_cast<siginfo_t*>(raw_info);

    // Try to handle an invalid access.
    // TODO: handle accesses which split a page?
    const Common::ProcessAddress addr =
        (reinterpret_cast<u64>(info->si_addr) & ~Memory::YUZU_PAGEMASK);
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();
    if (memory.InvalidateNCE(addr, Memory::YUZU_PAGESIZE)) {
        // We handled the access successfully and are returning to guest code.
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

void ArmNce::HandleHostAlignmentFault(int sig, void* raw_info, void* raw_context) {
    return g_orig_bus_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
}

void ArmNce::HandleHostAccessFault(int sig, void* raw_info, void* raw_context) {
    return g_orig_segv_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
}

bool ArmNce::HandleGuestIllegalInstruction(GuestContext* guest_ctx, void* raw_info,
                                           void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto& memory = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetMemory();

    const u32 inst = memory.Read32(host_ctx.pc);
    if (EmulatePointerAuthInstruction(inst, host_ctx, memory)) {
        return true;
    }

    // MRS Xt, CNTPCT_EL0 in code the patcher did not rewrite. Android kernels don't let EL0 read
    // the physical counter.
    if (auto mrs = NCE::MRS{inst}; mrs.Verify() && mrs.GetSystemReg() == NCE::CntpctEl0) {
        static std::atomic<u64> trapped{0};
        const u64 count = trapped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (std::has_single_bit(count)) {
            LOG_WARNING(Core_ARM, "Trapped {} reads of CNTPCT_EL0 (last at {:#x})", count,
                        host_ctx.pc);
        }
        // The same guest-frequency counter the patcher's CNTPCT handler returns.
        if (mrs.GetRt() != 31) {
            host_ctx.regs[mrs.GetRt()] = static_cast<u64>(Common::g_wall_clock.GetCNTPCT());
        }
        host_ctx.pc += 4;
        return true;
    }

    LOG_CRITICAL(Core_ARM, "Unsupported guest instruction {:08X} at {:#x}", inst, host_ctx.pc);
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

void ArmNce::HandleHostIllegalInstruction(int sig, void* raw_info, void* raw_context) {
    return g_orig_ill_action.sa_sigaction(sig, static_cast<siginfo_t*>(raw_info), raw_context);
}

void ArmNce::LockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(thread_params);
}

void ArmNce::UnlockThread(Kernel::KThread* thread) {
    auto* thread_params = &thread->GetNativeExecutionParameters();
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    m_guest_ctx.tpidrro_el0 = thread_params->tpidrro_el0;
    thread_params->native_context = nullptr;
    UnlockThreadParameters(thread_params);
}

HaltReason ArmNce::RunThread(Kernel::KThread* thread) {
    // Check if we're already interrupted.
    // If we are, we can just return immediately.
    HaltReason hr = static_cast<HaltReason>(m_guest_ctx.esr_el1.exchange(0));
    if (True(hr)) {
        return hr;
    }

    // Pre-fetch thread context data to improve cache locality
    auto* thread_params = &thread->GetNativeExecutionParameters();
    auto* process = thread->GetOwnerProcess();

    // Move non-critical operations outside the locked section
    const u64 tpidr_el0_cache = m_guest_ctx.tpidr_el0;
    const u64 tpidrro_el0_cache = m_guest_ctx.tpidrro_el0;

    // Critical section begins - minimize operations here
    m_running_thread = thread;
    m_guest_ctx.parent = this;
    thread_params->native_context = &m_guest_ctx;
    thread_params->tpidr_el0 = tpidr_el0_cache;
    thread_params->tpidrro_el0 = tpidrro_el0_cache;

    // Memory barrier to ensure visibility of changes
    std::atomic_thread_fence(std::memory_order_release);
    thread_params->is_running = true;

    // TODO: finding and creating the post handler needs to be locked
    // to deal with dynamic loading of NROs.
    const auto& post_handlers = process->GetPostHandlers();
    if (auto it = post_handlers.find(m_guest_ctx.pc); it != post_handlers.end()) {
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, it->second);
    } else {
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);  // Android: Use "process handle SIGUSR2 -n true -p true -s false" (and SIGURG) in LLDB when debugging
    }

    // Critical section for thread cleanup
    std::atomic_thread_fence(std::memory_order_acquire);

    // Cache values before releasing thread
    const u64 final_tpidr_el0 = thread_params->tpidr_el0;

    // Minimize critical section
    thread_params->is_running = false;
    thread_params->native_context = nullptr;
    m_running_thread = nullptr;

    // Non-critical updates can happen after releasing the thread
    m_guest_ctx.tpidr_el0 = final_tpidr_el0;

    // Return the halt reason.
    return hr;
}

HaltReason ArmNce::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

u32 ArmNce::GetSvcNumber() const {
    return m_guest_ctx.svc;
}

void ArmNce::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_guest_ctx.cpu_registers[i];
    }
}

void ArmNce::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; i++) {
        m_guest_ctx.cpu_registers[i] = args[i];
    }
}

ArmNce::ArmNce(System& system, bool uses_wall_clock, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_core_index{core_index} {
    m_guest_ctx.system = &m_system;
}

ArmNce::~ArmNce() = default;

void ArmNce::Initialize() {
    if (m_thread_id == -1) {
        m_thread_id = gettid();
    }

    // Configure signal stack.
    if (!m_stack) {
        m_stack = std::make_unique<u8[]>(StackSize);

        stack_t ss{};
        ss.ss_sp = m_stack.get();
        ss.ss_size = StackSize;
        sigaltstack(&ss, nullptr);
    }

    // Set up signals.
    static std::once_flag flag;
    std::call_once(flag, [] {
        using HandlerType = decltype(sigaction::sa_sigaction);

        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
        sigaddset(&signal_mask, BreakFromRunCodeSignal);
        sigaddset(&signal_mask, GuestAlignmentFaultSignal);
        sigaddset(&signal_mask, GuestAccessFaultSignal);
        sigaddset(&signal_mask, GuestIllegalInstructionSignal);

        struct sigaction return_to_run_code_action {};
        return_to_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        return_to_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(
            &ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
        return_to_run_code_action.sa_mask = signal_mask;
        Common::SigAction(ReturnToRunCodeByExceptionLevelChangeSignal, &return_to_run_code_action,
                          nullptr);

        struct sigaction break_from_run_code_action {};
        break_from_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        break_from_run_code_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
        break_from_run_code_action.sa_mask = signal_mask;
        Common::SigAction(BreakFromRunCodeSignal, &break_from_run_code_action, nullptr);

        struct sigaction alignment_fault_action {};
        alignment_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        alignment_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler);
        alignment_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAlignmentFaultSignal, &alignment_fault_action, nullptr);

        struct sigaction access_fault_action {};
        access_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        access_fault_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler);
        access_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAccessFaultSignal, &access_fault_action, &g_orig_segv_action);

        struct sigaction illegal_instruction_action {};
        illegal_instruction_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        illegal_instruction_action.sa_sigaction =
            reinterpret_cast<HandlerType>(&ArmNce::GuestIllegalInstructionSignalHandler);
        illegal_instruction_action.sa_mask = signal_mask;
        Common::SigAction(GuestIllegalInstructionSignal, &illegal_instruction_action,
                          &g_orig_ill_action);
    });
}

void ArmNce::SetTpidrroEl0(u64 value) {
    m_guest_ctx.tpidrro_el0 = value;
}

void ArmNce::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = m_guest_ctx.cpu_registers[i];
    }
    ctx.fp = m_guest_ctx.cpu_registers[29];
    ctx.lr = m_guest_ctx.cpu_registers[30];
    ctx.sp = m_guest_ctx.sp;
    ctx.pc = m_guest_ctx.pc;
    ctx.pstate = m_guest_ctx.pstate;
    ctx.v = m_guest_ctx.vector_registers;
    ctx.fpcr = m_guest_ctx.fpcr;
    ctx.fpsr = m_guest_ctx.fpsr;
    ctx.tpidr = m_guest_ctx.tpidr_el0;
}

void ArmNce::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; i++) {
        m_guest_ctx.cpu_registers[i] = ctx.r[i];
    }
    m_guest_ctx.cpu_registers[29] = ctx.fp;
    m_guest_ctx.cpu_registers[30] = ctx.lr;
    m_guest_ctx.sp = ctx.sp;
    m_guest_ctx.pc = ctx.pc;
    m_guest_ctx.pstate = ctx.pstate;
    m_guest_ctx.vector_registers = ctx.v;
    m_guest_ctx.fpcr = ctx.fpcr;
    m_guest_ctx.fpsr = ctx.fpsr;
    m_guest_ctx.tpidr_el0 = ctx.tpidr;
}

void ArmNce::SignalInterrupt(Kernel::KThread* thread) {
    constexpr u64 break_loop = static_cast<u64>(HaltReason::BreakLoop);
    if ((m_guest_ctx.esr_el1.fetch_or(break_loop) & break_loop) != 0) return;

    auto* params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    // Ensure visibility of is_running after lock acquire
    std::atomic_thread_fence(std::memory_order_acquire);

    if (params->is_running) {
        // We should signal to the running thread.
        // The running thread will unlock the thread context.
        syscall(SYS_tkill, m_thread_id, BreakFromRunCodeSignal);
    } else {
        // If the thread is no longer running, we have nothing to do.
        UnlockThreadParameters(params);
    }
}

[[maybe_unused]] const std::size_t CACHE_PAGE_SIZE = 4096;

void ArmNce::ClearInstructionCache() {
#ifdef __aarch64__
    // Ensure all previous memory operations complete
    asm volatile("dsb ish\n"
                 "dsb ish\n"
                 "isb" ::: "memory");
#endif
}

void ArmNce::InvalidateCacheRange(u64 addr, std::size_t size) {
    this->ClearInstructionCache();
}

} // namespace Core
