// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "core/hle/kernel/svc.h"
#include "core/arm/debug.h"
#include "core/arm/symbols.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nvn_direct_choice.h"
#include "core/nvn_hook.h"
#include "core/hle/service/nx2_image_layout.h"
#include "core/hle/service/nx2_native_image_lookup.h"
#include "core/hle/service/nx2_present.h"
#include "video_core/framebuffer_config.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#ifdef LICHII_NVN_DIRECT
#include "video_core/nvn_direct/nvn_direct.h"
#endif

namespace Core::NvnHook {
namespace {

using Dynarmic::A64::BlockEntryHook;

constexpr u32 MaxSlots = 4096;
/// What the game gets for nvnDeviceGetProcAddress (not exported: games get it from
/// nvnBootstrapLoader): a lookup like nvnBootstrapLoader's.
constexpr u32 LookupSlot = 0;
/// Where a guest call the hook makes after an NVN call returns (CallGuest).
constexpr u32 GuestCallReturnSlot = 1;
constexpr u32 FirstNameSlot = 2;

struct Slot {
    /// The front end's slot (-1: no implementation, the stub answers 0) and whether the call
    /// leaves the JIT first (it may block: HaltedCallSvc).
    s32 front = -1;
    bool halts{};
};

/// The calls that may block. They run from the kernel (RunHaltedCall), not inside the JIT's
/// callback: a kernel wait or signal there would switch fibers under a live Jit::Run.
constexpr std::array HaltingCalls{
    std::string_view{"nvnQueueSubmitCommands"}, std::string_view{"nvnQueuePresentTexture"},
    std::string_view{"nvnQueueFinish"},         std::string_view{"nvnSyncWait"},
    std::string_view{"nvnWindowAcquireTexture"}, std::string_view{"nvnQueueAcquireTexture"},
    std::string_view{"nvnMemoryPoolFinalize"},  std::string_view{"nvnTextureReadTexels"},
    std::string_view{"nvnTextureReadTexelsStrided"},
};

/// A direct present in the layer's frame history (ListFrameHistory): its frame number (the
/// numbering of GetLatestFrameNumber, from 1) and its guest ticks when the game queued it and when
/// it was shown (0 while it is not yet).
struct PresentedFrame {
    u64 number{};
    u64 queued_tick{};
    u64 shown_tick{};
};

/// How many presents nn::vi's layer history keeps: the SDK's ListFrameHistory answers this for a
/// null buffer.
constexpr size_t FrameHistorySize = 8;

struct State {
    u64 program_id{}; ///< DirectProgramId()
    std::map<u64, std::string> modules; ///< by base
    bool replay_started{};
    bool direct_ready{}; ///< nvn_direct_init ran
    bool first_image{};
    std::atomic<u64> halted_calls{};
    std::optional<Service::Nx2PresentBookkeeping> present;
    u64 presents{};

    Kernel::KProcess* process{};
    /// Lichii's guest memory (its memfd and backing views, for the replay's pool imports)
    Common::HostMemory* host_memory{};
    u64 synthetic_base{};
    /// nvnBootstrapLoader, every module's export: Luigi's Mansion 3 (Switch 1) exports it from a
    /// subsdk (nnSdkEn) and from nnSdk.
    std::vector<u64> bootstrap_entries;
    int sdk_major{-1}; ///< NintendoSDK major of the game's sdk (-1 unknown)
    u64 private0_address{};
    u64 private0_size{};

    std::array<Slot, MaxSlots> slots{};
    std::array<std::atomic<u64>, MaxSlots> stub_calls{};
    std::atomic<u32> slot_count{FirstNameSlot};

    std::mutex mutex; ///< slots' creation, lookups
    std::unordered_map<std::string, u32> slot_by_name;

    /// nn::vi::ListFrameHistory and nn::vi::GetLatestFrameNumber in the game's SDK (0: none),
    /// answered from the direct presents (ListFrameHistory, GetLatestFrameNumber)
    u64 list_frame_history{};
    u64 get_latest_frame_number{};
    std::mutex frames_mutex;
    std::array<PresentedFrame, FrameHistorySize> frames{}; ///< by number % FrameHistorySize
    u64 latest_frame{};
    bool frame_history_logged{};
};

/// EDEN_NX2_NVN_DIRECT=0/1 overrides the nvn_direct_renderer setting for a launch.
std::optional<bool> DirectOverride() {
    const char* const value = std::getenv("EDEN_NX2_NVN_DIRECT");
    if (value == nullptr || (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)) {
        return std::nullopt;
    }
    return value[0] == '1';
}

/// Whether this boot asks for the direct renderer (it may still fall back: Install).
bool DirectAsked() {
    return DirectOverride().value_or(Settings::values.nvn_direct_renderer.GetValue());
}

/// The boot's state, from Install to Uninstall (none unless the boot runs the direct renderer).
std::unique_ptr<State> g_state;
/// The direct renderer failed to start on the device in this process (DirectFailed).
bool g_replay_failed{};
bool g_not_reset{}; ///< an earlier boot's direct renderer state could not be reset (Uninstall)
bool g_relaunched{}; ///< this process was started for a game by a relaunch (MarkRelaunched)
RendererChoice g_choice{RendererChoice::SettingOff};

State* Get() {
    return g_state.get();
}

u64 SyntheticAddress(const State& state, u32 slot) {
    return state.synthetic_base + u64{slot} * 4;
}

bool IsBootstrap(const State& state, u64 address) {
    return std::ranges::find(state.bootstrap_entries, address) != state.bootstrap_entries.end();
}

BlockEntryHook HookOf(u64 pc) {
    const State* const state_pointer = Get();
    if (state_pointer == nullptr) {
        return BlockEntryHook::None;
    }
    const State& state = *state_pointer;
    if (IsBootstrap(state, pc)) {
        // nvnBootstrapLoader is answered here and the driver's never runs.
        return BlockEntryHook::Thunk;
    }
    if (pc != 0 && (pc == state.list_frame_history || pc == state.get_latest_frame_number)) {
        return BlockEntryHook::Thunk;
    }
    if (pc >= state.synthetic_base && (pc & 3) == 0 && state.synthetic_base != 0 &&
        (pc - state.synthetic_base) / 4 < state.slot_count.load(std::memory_order_acquire)) {
        return BlockEntryHook::Thunk;
    }
    return BlockEntryHook::None;
}

/// The game's inline NVN encoding stays off: its commands come through the proc table (the driver
/// sets nvnPrivate0 to 1 only once its lookup ran, which it never does now; written at each lookup).
void ClearPrivate0(const State& state, Core::Memory::Memory& memory) {
    if (state.private0_address == 0) {
        return;
    }
    switch (state.private0_size) {
    case 1:
        memory.Write8(state.private0_address, 0);
        break;
    case 2:
        memory.Write16(state.private0_address, 0);
        break;
    case 8:
        memory.Write64(state.private0_address, 0);
        break;
    default:
        memory.Write32(state.private0_address, 0);
        break;
    }
}

/// The NintendoSDK major [begin, end) was built with, from its version string
/// ("NintendoSdk_nnSdk-7_3_2-Release", a variant name such as "Generic_" may precede the version):
/// -1 when it has none. Read as upstream's loader.c dm_sdk_major does, but a prerelease SDK 0.x
/// (Breath of the Wild 1.6.0: "nnSdk-0_12_14") is 0, not unknown.
int SdkMajor(Core::Memory::Memory& memory, u64 begin, u64 end) {
    constexpr std::string_view Tag{"_nnSdk-"};
    constexpr std::string_view Sdk{"nintendosdk"};
    std::string bytes;
    for (u64 address = begin; address < end; address += Core::Memory::LICHII_PAGESIZE) {
        const u8* const page = memory.GetPointerSilent(Common::ProcessAddress{address});
        if (page == nullptr) {
            break;
        }
        bytes.append(reinterpret_cast<const char*>(page), Core::Memory::LICHII_PAGESIZE);
    }
    for (size_t at = bytes.find(Tag); at != std::string::npos; at = bytes.find(Tag, at + 1)) {
        if (at < Sdk.size() ||
            !std::ranges::equal(std::string_view{bytes}.substr(at - Sdk.size(), Sdk.size()), Sdk,
                                [](char a, char b) { return std::tolower(a) == b; })) {
            continue;
        }
        size_t version = at + Tag.size();
        while (version < bytes.size() && std::isalpha(static_cast<unsigned char>(bytes[version]))) {
            ++version;
        }
        if (version > at + Tag.size() && version < bytes.size() && bytes[version] == '_') {
            ++version;
        }
        if (version < bytes.size() && std::isdigit(static_cast<unsigned char>(bytes[version]))) {
            return std::atoi(bytes.c_str() + version);
        }
    }
    return -1;
}

bool ReadGuest(Core::Memory::Memory& memory, u64 address, void* out, size_t size) {
    auto* bytes = static_cast<u8*>(out);
    while (size != 0) {
        const size_t chunk = std::min<size_t>(size, 0x1000 - (address & 0xfff));
        const u8* const pointer = memory.GetPointerSilent(Common::ProcessAddress{address});
        if (pointer == nullptr) {
            return false;
        }
        std::memcpy(bytes, pointer, chunk);
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return true;
}

#ifdef LICHII_NVN_DIRECT
/// The answers of NVN2's own nvnDeviceGetInteger that are constants in its code, given to the front
/// end as device integers (dm_sdk_nvn2_item), read from the module at `base` that holds the driver
/// as upstream's loader.c nvn2_find_switch reads them. Its API and GPU code versions move with the
/// SDK and games check them: Ocarina of Time (SDK 22.2.8) requires API 231.40 (main+0x346c118)
/// and otherwise makes no graphics system. The driver's switch over the items loads w0 with the
/// major first, then jumps through a byte table: mov w0, #231 / adr (the cases) / adrp + add (the
/// table) / ldrb / br, with cmp wN, #<last item> before it; a constant case is mov w0, #v; b.
/// Returns how many items it found.
u32 ReadNvn2Constants(Kernel::KProcess& process, u64 base) {
    Kernel::KMemoryInfo info{};
    Kernel::Svc::PageInfo page{};
    if (process.GetPageTable()
            .QueryInfo(std::addressof(info), std::addressof(page), base)
            .IsError()) {
        return 0;
    }
    const u64 text_end = info.GetSvcMemoryInfo().base_address + info.GetSvcMemoryInfo().size;
    const u64 end = GetInteger(Core::GetModuleEnd(&process, base)) + 1;
    std::vector<u32> words((end - base) / 4);
    if (text_end <= base || !ReadGuest(process.GetMemory(), base, words.data(), words.size() * 4)) {
        return 0;
    }
    const auto* const bytes = reinterpret_cast<const u8*>(words.data());
    const s64 text_size = static_cast<s64>(text_end - base);
    const s64 size = static_cast<s64>(words.size() * 4);
    // adr and adrp's signed 21-bit immediate
    const auto immediate = [](u32 word) {
        const s64 value = ((word >> 3) & 0x1ffffc) | ((word >> 29) & 3);
        return (value & 0x100000) != 0 ? value - 0x200000 : value;
    };
    for (s64 p = 8; p + 9 < text_size / 4; ++p) {
        if (words[p] != 0x52801ce0) { // mov w0, #231
            continue;
        }
        s64 table = -1, cases = -1, last = -1;
        bool ldrb = false, br = false;
        for (s64 q = p - 8; q <= p + 8; ++q) {
            const u32 word = words[q];
            if ((word & 0xffc0001f) == 0x7100001f) { // cmp wN, #imm
                last = (word >> 10) & 0xfff;
            } else if ((word & 0x9f000000) == 0x90000000 &&
                       (words[q + 1] & 0xffc00000) == 0x91000000) { // adrp + add
                table = ((q * 4) & ~s64{0xfff}) + immediate(word) * 4096 +
                        ((words[q + 1] >> 10) & 0xfff);
            } else if ((word & 0x9f000000) == 0x10000000) { // adr
                cases = q * 4 + immediate(word);
            } else if ((word & 0xffe0fc00) == 0x38606800) { // ldrb wK, [xT, xI]
                ldrb = true;
            } else if ((word & 0xfffffc1f) == 0xd61f0000) { // br
                br = true;
            }
        }
        if (last <= 1 || !ldrb || !br || table < text_size || table + last >= size || cases < 0 ||
            cases % 4 != 0 || cases + 255 * 4 + 8 > text_size) {
            continue;
        }
        u32 found = 0;
        for (s64 item = 0; item <= last; ++item) {
            const s64 at = cases / 4 + bytes[table + item];
            if ((words[at] & 0xffe0001f) == 0x52800000 &&     // mov w0, #imm16
                (words[at + 1] & 0xfc000000) == 0x14000000) { // b
                nvn_direct_set_device_integer(static_cast<unsigned>(item),
                                              static_cast<int>((words[at] >> 5) & 0xffff));
                ++found;
            }
        }
        return found;
    }
    return 0;
}
#endif

/// At Install: a slot per name the front end knows (upstream's names.txt) and, for NVN2, the
/// device integers that are constants in the driver's code.
void InstallDirect(State& state) {
#ifdef LICHII_NVN_DIRECT
    for (int front = 0; front < nvn_direct_name_count(); ++front) {
        const std::string name = nvn_direct_name(front);
        const u32 slot = state.slot_count.load(std::memory_order_relaxed);
        state.slots[slot] = {.front = front,
                             .halts = std::ranges::find(HaltingCalls, name) != HaltingCalls.end()};
        if (state.slots[slot].halts && nvn_direct_float_args(front)) {
            LOG_CRITICAL(Core, "NVN direct: {} takes floats, which a halted call loses", name);
        }
        state.slot_by_name.emplace(name, slot);
        state.slot_count.store(slot + 1, std::memory_order_release);
    }
    // NVN2 is compiled into the module that exports nvnBootstrapLoader.
    u32 constants = 0;
    if (state.process->IsSwitch2()) {
        for (const u64 entry : state.bootstrap_entries) {
            constants = ReadNvn2Constants(*state.process,
                                          std::prev(state.modules.upper_bound(entry))->first);
            if (constants != 0) {
                break;
            }
        }
        if (constants == 0) {
            LOG_WARNING(Core, "NVN direct: NVN2's nvnDeviceGetInteger switch is not in the game's "
                              "driver: the device answers the newest versions known");
        }
    }
    LOG_INFO(Core,
             "NVN direct: the game's NVN driver is replaced: {} names in the front end, {} device "
             "integers from its code",
             state.slot_count.load() - FirstNameSlot, constants);
#endif
}

#ifdef LICHII_NVN_DIRECT
/// Direct mode: the slot answering `name` (a stub slot for a name the front end lacks).
u32 DirectSlotOf(State& state, const std::string& name) {
    if (name == "nvnDeviceGetProcAddress") {
        return LookupSlot;
    }
    if (const auto it = state.slot_by_name.find(name); it != state.slot_by_name.end()) {
        return it->second;
    }
    const u32 slot = state.slot_count.load(std::memory_order_relaxed);
    if (slot == MaxSlots) {
        return MaxSlots;
    }
    state.slots[slot] = {};
    state.slot_by_name.emplace(name, slot);
    state.slot_count.store(slot + 1, std::memory_order_release);
    LOG_WARNING(Core, "NVN direct: the game asks for {}, which the front end lacks: a stub", name);
    return slot;
}

/// nvn_direct_memory.map_view: the pages behind guest [address, address + size)
/// mapped once more at a range of their own, for the replay to import: each run of pages that lie
/// one after the other in the backing memory is one mapping of it (HostMemory::MapBackingView);
/// pages the guest has not mapped are zero pages of the view.
void* MapPoolView(void* context, u64 address, u64 size, u64* unmapped) {
    State& state = *static_cast<State*>(context);
    auto& memory = state.process->GetMemory();
    const u8* const backing_base = state.host_memory->BackingBasePointer();
    constexpr u64 PageSize = Core::Memory::LICHII_PAGESIZE;
    std::vector<Common::HostMemory::BackingRun> runs;
    for (u64 offset = 0; offset < size; offset += PageSize) {
        const u8* const backing = memory.GetPointerSilent(address + offset);
        if (backing == nullptr) {
            *unmapped += PageSize;
            continue;
        }
        if (backing < backing_base ||
            static_cast<size_t>(backing - backing_base) >= state.host_memory->BackingSize()) {
            LOG_ERROR(Core, "NVN direct: view of guest {:#x}+{:#x}: page {:#x} is outside the "
                      "backing memory", address, size, address + offset);
            return nullptr;
        }
        const size_t host_offset = static_cast<size_t>(backing - backing_base);
        if (!runs.empty() && runs.back().view_offset + runs.back().length == offset &&
            runs.back().host_offset + runs.back().length == host_offset) {
            runs.back().length += PageSize;
            continue;
        }
        runs.push_back({.view_offset = offset, .host_offset = host_offset, .length = PageSize});
    }
    u8* const view = state.host_memory->MapBackingView(size, runs);
    if (view == nullptr) {
        LOG_ERROR(Core, "NVN direct: no view of guest {:#x}+{:#x} ({} runs)", address, size,
                  runs.size());
    }
    return view;
}

/// nvn_direct_memory.unmap_view: takes down a view MapPoolView made.
void UnmapPoolView(void* context, void* view, u64 size) {
    static_cast<State*>(context)->host_memory->UnmapBackingView(static_cast<u8*>(view), size);
}

/// nvn_direct_memory.track_pages: arms or disarms the write tracking of pool pages by guest
/// address: rasterizer-cached pages, which stay readable in direct mode (Core::Memory); a write
/// to one reaches OnDirectCpuWrite. The pools stay out of the SMMU: with them mapped there MKW
/// stalls.
int TrackPages(void* context, u64 address, u64 size, int armed) {
    return static_cast<State*>(context)->process->GetMemory().RasterizerMarkRegionCached(
        address, size, armed != 0);
}

/// nvn_direct_memory.backing: the backing memory of a guest page (never write-protected).
void* Backing(void* context, u64 address) {
    State& state = *static_cast<State*>(context);
    return state.process->GetMemory().GetPointerSilent(address);
}

/// nvn_direct_memory's native_image: nx2:gsv's images.
int NativeImage(void*, u64 handle, u64* address, u64* size) {
    auto& lookup = Service::g_nx2_native_image_lookup;
    std::scoped_lock lock{lookup.mutex};
    if (!lookup.range || handle > UINT32_MAX) {
        return 0;
    }
    const auto range = lookup.range(static_cast<u32>(handle));
    if (!range) {
        return 0;
    }
    *address = range->first;
    *size = range->second;
    return 1;
}

#ifdef _WIN32
/// nvn_direct_memory's alias calls: HostMemory's alias regions.
void* AliasReserve(void* context, u64 size) {
    return static_cast<State*>(context)->host_memory->ReserveAliasRegion(size);
}
int AliasMap(void* context, void* address, u64 backing_offset, u64 size) {
    return static_cast<State*>(context)->host_memory->MapAlias(static_cast<u8*>(address),
                                                               backing_offset, size);
}
void AliasUnmap(void* context, void* address, u64 size) {
    static_cast<State*>(context)->host_memory->UnmapAlias(static_cast<u8*>(address), size);
}
void AliasRelease(void* context, void* region) {
    static_cast<State*>(context)->host_memory->ReleaseAliasRegion(static_cast<u8*>(region));
}
#endif

/// The guest's x0-x7 and the low halves of v0-v7.
void ReadArguments(Dynarmic::A64::Jit& jit, std::array<u64, 8>& x, std::array<u64, 8>& v) {
    for (size_t i = 0; i < 8; ++i) {
        x[i] = jit.GetRegister(i);
        v[i] = jit.GetVector(i)[0];
    }
}

/// The slot of a call that halted the JIT (HaltedCallSvc), on the host thread that runs it: the
/// kernel runs it (RunHaltedCall) right after the JIT returns, on the same host thread.
thread_local u32 t_halted_slot = MaxSlots;
thread_local u64 t_halted_lr = 0;

/// nn::vi::ListFrameHistory(FrameInfo* out, int count, const Layer*) of the game's SDK, answered
/// from the direct presents (port of seedds/nxnative's dm/gfx/hle_vi.c). The SDK's version reads
/// the history its layer's native window keeps as the NVN driver presents to it, which a replaced
/// driver never does: Unreal's Switch RHI (Orbitals' Viewport Thread, main+0x1540460) learns its
/// flips only from it, so with no frames listed its frame-end sync waited for ever right after the
/// Unreal logo. A 40-byte FrameInfo per present, newest first: status (2 shown, 1 queued), frame
/// number, queued tick and shown tick twice. A null buffer asks for the history's size.
int ListFrameHistory(State& state, Core::Memory::Memory& memory, u64 out, s32 count) {
    if (out == 0 || count <= 0) {
        return static_cast<int>(FrameHistorySize);
    }
    std::array<std::array<u64, 5>, FrameHistorySize> infos{};
    int listed = 0;
    {
        std::scoped_lock lock{state.frames_mutex};
        for (u64 number = state.latest_frame;
             number > 0 && state.latest_frame - number < FrameHistorySize && listed < count;
             --number) {
            const PresentedFrame& frame = state.frames[number % FrameHistorySize];
            if (frame.number != number) {
                continue;
            }
            infos[listed++] = {frame.shown_tick != 0 ? 2U : 1U, number, frame.queued_tick,
                               frame.shown_tick, frame.shown_tick};
        }
        if (listed != 0 && !std::exchange(state.frame_history_logged, true)) {
            LOG_INFO(Core, "NVN direct: nn::vi::ListFrameHistory answered from the direct "
                           "presents ({} of the last {}, up to frame {})",
                     listed, FrameHistorySize, state.latest_frame);
        }
    }
    memory.WriteBlock(out, infos.data(), listed * sizeof(infos[0]));
    return listed;
}

/// nn::vi::GetLatestFrameNumber(u64* out, const Layer*): the frames presented so far, numbered as
/// ListFrameHistory lists them.
void GetLatestFrameNumber(State& state, Core::Memory::Memory& memory, u64 out) {
    u64 latest;
    {
        std::scoped_lock lock{state.frames_mutex};
        latest = state.latest_frame;
    }
    memory.Write64(out, latest);
}

/// Runs the guest call an NVN call owes the game (nvn_direct_guest_call_take) as the call returns:
/// the JIT goes on at the guest function, on the calling guest thread, with the call's x0 and the
/// return address (lr) its caller expects pushed on the guest's stack, and returns to
/// GuestCallReturnSlot, which pops them and returns to the caller as the NVN call would have. The
/// guest function may make NVN calls and block like any guest code. The caller sees the call's
/// x0 but not its v0: the calls that owe one (command recording) return no float.
void CallGuest(State& state, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
               const nvn_direct_guest_call& call, u64 result, u64 lr) {
    const u64 sp = jit.GetSP() - 16;
    memory.Write64(sp, result);
    memory.Write64(sp + 8, lr);
    jit.SetSP(sp);
    for (size_t i = 0; i < std::size(call.x); ++i) {
        jit.SetRegister(i, call.x[i]);
    }
    jit.SetRegister(30, SyntheticAddress(state, GuestCallReturnSlot));
    jit.SetRegister(16, call.function);
}

/// GuestCallReturnSlot: the guest function CallGuest ran returned.
void ReturnFromGuestCall(Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory) {
    const u64 sp = jit.GetSP();
    const u64 lr = memory.Read64(sp + 8);
    jit.SetRegister(0, memory.Read64(sp));
    jit.SetSP(sp + 16);
    jit.SetRegister(30, lr);
    jit.SetRegister(16, lr);
}

/// Direct mode's block entries. Returns whether the JIT halts for the call (HaltedCallSvc).
bool DirectEntry(State& state, u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory) {
    const u64 lr = jit.GetRegister(30);
    if (pc == state.list_frame_history || pc == state.get_latest_frame_number) {
        if (pc == state.list_frame_history) {
            jit.SetRegister(0, static_cast<u64>(ListFrameHistory(
                                   state, memory, jit.GetRegister(0),
                                   static_cast<s32>(jit.GetRegister(1)))));
        } else {
            GetLatestFrameNumber(state, memory, jit.GetRegister(0));
            jit.SetRegister(0, 0); // ResultSuccess
        }
        jit.SetRegister(16, lr);
        return false;
    }
    const bool bootstrap = IsBootstrap(state, pc);
    const u32 slot = bootstrap ? MaxSlots : static_cast<u32>((pc - state.synthetic_base) / 4);
    if (bootstrap || slot == LookupSlot) {
        // nvnBootstrapLoader(name) or nvnDeviceGetProcAddress(device, name): the name's
        // synthetic address, without the driver.
        const std::string name = memory.ReadCString(jit.GetRegister(bootstrap ? 0 : 1), 128);
        std::scoped_lock lock{state.mutex};
        if (!state.direct_ready) {
            auto& page_table = state.process->GetPageTable();
            const nvn_direct_memory views{.map_view = MapPoolView,
                                          .unmap_view = UnmapPoolView,
                                          .track_pages = TrackPages,
                                          .backing = Backing,
                                          .native_image = NativeImage,
                                          .context = &state,
                                          .backing_fd = state.host_memory->BackingFd(),
                                          .backing_base = state.host_memory->BackingBasePointer(),
#ifdef _WIN32
                                          .alias_reserve = AliasReserve,
                                          .alias_map = AliasMap,
                                          .alias_unmap = AliasUnmap,
                                          .alias_release = AliasRelease,
#endif
            };
            nvn_direct_init(page_table.GetImpl().fastmem_arena,
                            u64{1} << page_table.GetAddressSpaceWidth(), &views,
                            state.process->IsSwitch2(), state.sdk_major);
            state.direct_ready = true;
        }
        const u32 found = DirectSlotOf(state, name);
        jit.SetRegister(0, found == MaxSlots ? 0 : SyntheticAddress(state, found));
        ClearPrivate0(state, memory);
        jit.SetRegister(16, lr);
        return false;
    }
    if (slot == GuestCallReturnSlot) {
        ReturnFromGuestCall(jit, memory);
        return false;
    }
    const Slot& target = state.slots[slot];
    jit.SetRegister(16, lr);
    if (target.front < 0 || !nvn_direct_implemented(target.front)) {
        if (state.stub_calls[slot].fetch_add(1, std::memory_order_relaxed) == 0) {
            std::scoped_lock lock{state.mutex};
            for (const auto& [name, index] : state.slot_by_name) {
                if (index == slot) {
                    LOG_WARNING(Core, "NVN direct: {} has no implementation: answered 0", name);
                }
            }
        }
        jit.SetRegister(0, 0);
        return false;
    }
    if (target.halts) {
        t_halted_slot = slot;
        t_halted_lr = lr;
        return true;
    }
    std::array<u64, 8> x, v;
    ReadArguments(jit, x, v);
    u64 v0 = 0;
    const u64 result = nvn_direct_call(target.front, x.data(), v.data(), jit.GetSP(), lr, &v0);
    jit.SetRegister(0, result);
    if (nvn_direct_float_args(target.front)) {
        jit.SetVector(0, {v0, 0});
    }
    if (nvn_direct_guest_call call; nvn_direct_guest_call_take(&call)) {
        CallGuest(state, jit, memory, call, result, lr);
    }
    return false;
}
#endif

} // namespace

bool DirectMode() {
    return Get() != nullptr;
}

u64 DirectProgramId() {
    const State* const state = Get();
    return state != nullptr ? state->program_id : 0;
}

void StopDirect() {
#ifdef LICHII_NVN_DIRECT
    if (const auto* state = Get(); state && state->replay_started) {
        nvn_direct_stop();
    }
#endif
}

void TraceStartup(std::string_view phase) {
#ifdef LICHII_NVN_DIRECT
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_NVN_STARTUP_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled || !DirectAsked()) {
        return;
    }
    const auto memory = Common::GetProcessMemoryUsage();
    const auto physical = Common::GetAvailablePhysicalMemory();
    constexpr auto mib = [](u64 bytes) { return bytes >> 20; };
    const auto commit_room = memory.system_commit_limit != 0
                                 ? fmt::to_string(mib(memory.system_commit_limit -
                                                      std::min(memory.system_commit,
                                                               memory.system_commit_limit)))
                                 : "unavailable";
#ifdef _WIN32
    constexpr auto process_counter = "private commit";
#else
    constexpr auto process_counter = "data size";
#endif
    LOG_INFO(Core,
             "NVN startup memory ({}): process {} {} MiB, working set {} MiB, "
             "system commit headroom {} MiB, available physical RAM {} MiB",
             phase, process_counter, memory.commit ? fmt::to_string(mib(memory.commit)) : "unavailable",
             memory.working_set ? fmt::to_string(mib(memory.working_set)) : "unavailable",
             commit_room, physical ? fmt::to_string(mib(physical)) : "unavailable");
#else
    (void)phase;
#endif
}

namespace {

/// The NVN driver's exports in the process's modules, the game's SDK and the synthetic
/// addresses' range.
void ScanModules(Core::System& system, Kernel::KProcess& process, State& state) {
    auto& memory = process.GetMemory();
    const auto modules = Core::FindModules(&process);
    for (const auto& [base, module] : modules) {
        state.modules.emplace(base, module);
        const auto symbols = Core::Symbols::GetSymbols(base, memory, true);
        const auto find = [&](std::string_view name) -> std::pair<u64, u64> {
            const auto it = symbols.find(name);
            if (it == symbols.end() || it->second.first == 0) {
                return {0, 0};
            }
            return {base + it->second.first, it->second.second};
        };
        if (const auto [address, size] = find("nvnBootstrapLoader"); address != 0) {
            state.bootstrap_entries.push_back(address);
            if (state.sdk_major < 0) {
                state.sdk_major =
                    SdkMajor(memory, base, GetInteger(Core::GetModuleEnd(&process, base)));
            }
            LOG_INFO(Core, "NVN hook: nvnBootstrapLoader in module {} at {:#x}: {:#x}", module,
                     base, address);
        }
        if (const auto [address, size] = find("nvnPrivate0"); address != 0) {
            state.private0_address = address;
            state.private0_size = size;
        }
        if (const auto [address, size] =
                find("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE");
            address != 0) {
            state.list_frame_history = address;
        }
        if (const auto [address, size] = find("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE");
            address != 0) {
            state.get_latest_frame_number = address;
        }
    }
    // Older SDKs ship the driver as a module of its own (Breath of the Wild 1.6.0: "nvn", which
    // exports nvnBootstrapLoader); the version string is then in nnSdk's module.
    for (auto it = modules.begin(); state.sdk_major < 0 && !state.bootstrap_entries.empty() &&
                                    it != modules.end();
         ++it) {
        state.sdk_major =
            SdkMajor(memory, it->first, GetInteger(Core::GetModuleEnd(&process, it->first)));
    }
    // The synthetic addresses: in the first unmapped range from 64 KiB that holds every slot.
    // Nothing is ever mapped executable there, and the JIT reads no guest code for them.
    auto& page_table = process.GetPageTable();
    for (u64 address = 0x10000;;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        if (page_table.QueryInfo(std::addressof(info), std::addressof(page), address).IsError()) {
            break;
        }
        const auto svc = info.GetSvcMemoryInfo();
        const u64 end = svc.base_address + svc.size;
        if (svc.state == Kernel::Svc::MemoryState::Free && end - address >= u64{MaxSlots} * 4) {
            state.synthetic_base = address;
            break;
        }
        if (end <= address) {
            break;
        }
        address = end;
    }
    state.process = &process;
    state.host_memory = &system.DeviceMemory().buffer;
    state.program_id = process.GetProgramId();
}

} // namespace

void Install(Core::System& system, Kernel::KProcess& process) {
#ifdef LICHII_NVN_DIRECT
    constexpr bool built = true;
#else
    constexpr bool built = false;
#endif
    g_state.reset();
    const bool asked = DirectAsked();
    // The hook's block entries are A64 only (ArmDynarmic64): a 32-bit game gets no state.
    std::unique_ptr<State> state_owner =
        built && asked && process.Is64Bit() ? std::make_unique<State>() : nullptr;
    if (state_owner != nullptr) {
        ScanModules(system, process, *state_owner);
    }
    g_choice = ChooseRenderer({
        .built = built,
        .override_ = DirectOverride(),
        .setting = Settings::values.nvn_direct_renderer.GetValue(),
        .vulkan = Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan,
        .is_64bit = process.Is64Bit(),
        .has_nvn = state_owner != nullptr && !state_owner->bootstrap_entries.empty(),
        .fastmem = process.GetPageTable().GetImpl().fastmem_arena != nullptr,
        .replay_failed = g_replay_failed,
        .not_reset = g_not_reset,
    });
    if (state_owner == nullptr || !IsDirect(g_choice)) {
        if (built || IsFallback(g_choice)) {
            LOG_INFO(Core, "Renderer: GPU emulation ({})", Describe(g_choice));
        }
        return;
    }
    LOG_INFO(Core, "Renderer request: NVN direct renderer ({})", Describe(g_choice));
    State& state = *state_owner;
    InstallDirect(state);
    LOG_INFO(Core,
             "NVN hook: {} nvnBootstrapLoader exports (SDK {}), nvnPrivate0 {:#x} ({} bytes), "
             "synthetic addresses from {:#x}",
             state.bootstrap_entries.size(), state.sdk_major, state.private0_address,
             state.private0_size, state.synthetic_base);
    g_state = std::move(state_owner);
}

void DirectStarted() {
    Get()->replay_started = true;
    LOG_INFO(Core, "Renderer: NVN direct renderer ({})", Describe(g_choice));
    TraceStartup("replay started");
}

void DirectFailed() {
    g_replay_failed = true;
    g_choice = RendererChoice::ReplayFailed;
    LOG_WARNING(Core, "Renderer: GPU emulation ({})", Describe(g_choice));
    TraceStartup("replay refused");
    // The game has not run yet: without a state its NVN driver runs, on GPU emulation.
    g_state.reset();
}

void Uninstall() {
    g_state.reset();
#ifdef LICHII_NVN_DIRECT
    // The front end and the replay as before the first boot (their threads were joined and their
    // objects destroyed with the renderer): the next boot starts them again. When they cannot be
    // reset, later boots fall back to GPU emulation.
    if (IsDirect(g_choice) && !nvn_direct_reset()) {
        g_not_reset = true;
    }
#endif
}

RendererChoice Choice() {
    return g_choice;
}

bool DirectRequested() {
#ifdef LICHII_NVN_DIRECT
    return DirectAsked() &&
           Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
#else
    return false;
#endif
}

void MarkRelaunched() {
    g_relaunched = true;
}

bool DirectNeedsFreshProcess() {
#ifdef LICHII_NVN_DIRECT
    return NeedsFreshProcess({
        .direct_asked = DirectAsked(),
        .vulkan = Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan,
        .replay_failed = g_replay_failed,
        .not_reset = g_not_reset,
        .relaunched = g_relaunched,
    });
#else
    return false;
#endif
}

Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc) {
    // Asked for when the process's JIT is made, before Install decides: HookOf answers nothing
    // while the boot has no state.
    return DirectAsked() ? &HookOf : nullptr;
}

bool OnBlockEntry(u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
                  Kernel::KProcess* process) {
    State& state = *Get();
    if (process != state.process) {
        return false;
    }
#ifdef LICHII_NVN_DIRECT
    return DirectEntry(state, pc, jit, memory);
#else
    return false;
#endif
}

#ifdef LICHII_NVN_DIRECT
namespace {
/// Every 600 presents: the halted calls and the stub census (names called without an
/// implementation).
void LogStubCensus(State& state, u64 present) {
    if (present % 600 != 0) {
        return;
    }
    std::string census;
    std::scoped_lock lock{state.mutex};
    for (const auto& [name, slot] : state.slot_by_name) {
        if (const u64 calls = state.stub_calls[slot].load(std::memory_order_relaxed); calls) {
            census += fmt::format(" {}={}", name, calls);
        }
    }
    LOG_INFO(Core, "NVN direct: present {}: halted calls {}, stubs called:{}", present,
             state.halted_calls.load(), census.empty() ? " none" : census);
}

/// Direct mode's present (nvnQueuePresentTexture): the replay's image goes to
/// Lichii's presenter as a layer whose address names the frame (AccelerateDisplay finds it), and
/// the present's bookkeeping is nx2:vi2's (Service::Nx2PresentBookkeeping): counts, capture,
/// speed limiter, perf stats, pacing to the vblank, and as nx2:vi2's Composite the display
/// HDR-change check.
void PresentDirect(Core::System& system, State& state) {
    Service::Nx2NotifyHdrChange(system.Kernel());
    if (!state.present) {
        state.present.emplace(system);
    }
    auto& present = *state.present;
    const u64 own = ++state.presents;
    const u64 number = present.Count(Kernel::GetCurrentThread(system.Kernel()));
    {
        std::scoped_lock lock{state.frames_mutex};
        state.frames[own % FrameHistorySize] = {.number = own,
                                                .queued_tick = system.CoreTiming().GetClockTicks()};
        state.latest_frame = own;
    }
    nvn_direct_frame frame{};
    const bool acquired = nvn_direct_present_take(&frame) != 0;
    if (own <= 4 || own % 600 == 0) {
        LOG_INFO(Core, "NVN direct: present #{} ({}x{} VkFormat {}, {})", own, frame.width,
                 frame.height, static_cast<int>(frame.format),
                 acquired ? "image acquired" : "no image");
    }
    if (acquired) {
        if (!std::exchange(state.first_image, true)) {
            LOG_INFO(Core, "NVN direct: first replay image acquired at guest present #{}", own);
            TraceStartup("first replay image");
        }
        // The game's present waits for the previous frame's composite (as many images as the
        // window has are in flight between the replay and Lichii's presenter).
        system.GPU().WaitForComposite();
        u32 crop_width = frame.crop[2] > 0 ? static_cast<u32>(frame.crop[2]) : frame.width;
        u32 crop_height = frame.crop[3] > 0 ? static_cast<u32>(frame.crop[3]) : frame.height;
        if (frame.crop[2] <= 0 && !Settings::IsDockedMode()) {
            // As nx2:vi2: a handheld frame is the top-left 1920x1080 of the image.
            crop_width = (std::min)(crop_width, 1920U);
            crop_height = (std::min)(crop_height, 1080U);
        }
        // As nx2:vi2, the presenter converts by the image's format and colour space: Mario Kart
        // World told HDR presents PQ in RGB10A2.
        const auto image_format =
            frame.format == VK_FORMAT_R16G16B16A16_SFLOAT ? Service::Nx2NativeImageFormat::Rgba16Float
            : frame.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                ? Service::Nx2NativeImageFormat::Rgb10A2
                : Service::Nx2NativeImageFormat::Rgba8;
        std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
            .address = NVN_DIRECT_FRAME_TAG | frame.id,
            .width = frame.width,
            .height = frame.height,
            .stride = frame.width,
            .pixel_format = image_format == Service::Nx2NativeImageFormat::Rgba16Float
                                ? Service::android::PixelFormat::RgbaFp16
                            : image_format == Service::Nx2NativeImageFormat::Rgb10A2
                                ? Service::android::PixelFormat::Rgba1010102
                                : Service::android::PixelFormat::Rgba8888,
            .transform_flags = Service::android::BufferTransformFlags::Unset,
            .crop_rect = {frame.crop[0], frame.crop[1],
                          frame.crop[0] + static_cast<int>(crop_width),
                          frame.crop[1] + static_cast<int>(crop_height)},
            .blending = Tegra::BlendMode::Opaque,
            .color_space = Service::Nx2NativeImageColorSpace(image_format,
                                                             Service::g_nx2_told_hdr.load()),
        }};
        system.GPU().RequestComposite(std::move(layers), {});
    }
    present.Finish(own);
    present.PaceToVblank((frame.interval << 16) | 60, system.ApplicationProcess(), own, nullptr);
    {
        // Shown once paced to its vblank, as nx2:vi2's present records its timestamp recorder.
        std::scoped_lock lock{state.frames_mutex};
        state.frames[own % FrameHistorySize].shown_tick = system.CoreTiming().GetClockTicks();
    }
    LogStubCensus(state, number);
}
} // namespace
#endif

void OnDirectCpuWrite(u64 address, u64 size) {
#ifdef LICHII_NVN_DIRECT
    nvn_direct_cpu_written(address, size);
#else
    (void)address;
    (void)size;
#endif
}

bool OnGuestUnmap(const Common::PageTable& page_table, u64 address, u64 size) {
#ifdef LICHII_NVN_DIRECT
    const State* const state = Get();
    if (state != nullptr && state->direct_ready &&
        &page_table == &state->process->GetPageTable().GetImpl()) {
        nvn_direct_guest_unmap(address, size);
        return true;
    }
#else
    (void)page_table;
    (void)address;
    (void)size;
#endif
    return false;
}

void OnGuestUnmapDone() {
#ifdef LICHII_NVN_DIRECT
    nvn_direct_guest_unmap_done();
#endif
}

void RunHaltedCall(Core::System& system, std::array<u64, 8>& args) {
#ifdef LICHII_NVN_DIRECT
    State& state = *Get();
    const u32 slot = std::exchange(t_halted_slot, MaxSlots);
    if (slot == MaxSlots) {
        LOG_CRITICAL(Core, "NVN direct: a halted call without its slot");
        return;
    }
    state.halted_calls.fetch_add(1, std::memory_order_relaxed);
    const Slot& target = state.slots[slot];
    // A wait for the replay's GPU (SyncWait, QueueFinish) or for a composite still reading the
    // image a window acquire hands out sleeps in the guest kernel until it would not wait: a host
    // wait here held the guest core, so the game's other threads there stopped (one of them may
    // be the one whose submission the GPU waits for).
    const bool sync_wait = std::string_view{nvn_direct_name(target.front)} == "nvnSyncWait";
    bool waited = false;
    if (nvn_direct_call_blocks(target.front, args.data())) {
        auto& kernel = system.Kernel();
        const s64 start = kernel.HardwareTimer().GetTick();
        const u64 timeout = sync_wait ? args[1] : ~u64{0};
        s64 slice = 50'000;
        while (nvn_direct_call_blocks(target.front, args.data())) {
            const s64 now = kernel.HardwareTimer().GetTick();
            if (timeout != ~u64{0} && static_cast<u64>(now - start) >= timeout) {
                break;
            }
            static_cast<void>(Kernel::GetCurrentThread(kernel).Sleep(kernel, now + slice));
            slice = (std::min)(slice * 2, s64{1'000'000});
        }
        waited = true;
        if (sync_wait) {
            args[1] = 0; // (answered from the state now, without a host wait)
        }
    }
    // No halting call takes or returns floats (checked at the first one), and none of HaltingCalls
    // takes arguments on the stack (sp 0).
    const std::array<u64, 8> v{};
    u64 v0 = 0;
    args[0] = nvn_direct_call(target.front, args.data(), v.data(), 0, t_halted_lr, &v0);
    if (sync_wait && waited && args[0] == 0) {
        args[0] = 1; // NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED: it was signalled while waiting
    }
    if (std::string_view{nvn_direct_name(target.front)} != "nvnQueuePresentTexture") {
        return;
    }
    PresentDirect(system, state);
#else
    (void)system;
    (void)args;
#endif
}

} // namespace Core::NvnHook
