// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN proc-table hook for the NVN direct renderer. Games get every NVN function but
// nvnBootstrapLoader by name, through nvnBootstrapLoader (exported by the NVN driver module,
// `sdk`) or nvnDeviceGetProcAddress (which they get from nvnBootstrapLoader). The hook stands at
// nvnBootstrapLoader (found by its export name, in any game) and hands the game a synthetic
// address per name instead of the driver's; the JIT translates a block at a synthetic address as
// a host call that runs the direct renderer's front end (video_core/nvn_direct) and returns to
// the caller. The same boundary nxnative (seedds, MIT) and Switch2Recomp's census hook stand at;
// this code is Lichii's own.
//
// Chosen per boot by Install from the nvn_direct_renderer setting (EDEN_NX2_NVN_DIRECT=0/1
// overrides it; see nvn_direct_choice.h). The game's NVN driver never runs: names the front end
// lacks get a stub that answers 0. Calls that may block halt the JIT and run from the kernel
// (RunHaltedCall). Every 600 presents a line counts the halted calls and the stubs called.
// nn::vi::ListFrameHistory and GetLatestFrameNumber, which read what the replaced driver's
// presents would have recorded, are answered the same way, from the direct presents. A guest
// function a call would have called on the console (the game's command memory callback) runs once
// the call returns, on the same guest thread (CallGuest).

#pragma once

#include <array>

#include "common/common_types.h"
#include "core/nvn_direct_choice.h"

namespace Dynarmic::A64 {
class Jit;
enum class BlockEntryHook : std::uint8_t;
} // namespace Dynarmic::A64

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Common {
struct PageTable;
}

namespace Kernel {
class KProcess;
}

namespace Core::NvnHook {

/// NVN direct mode: the game's NVN calls run in the direct renderer
/// (video_core/nvn_direct), not in its NVN driver. Decided once per boot by Install; read by
/// the Vulkan device, created after Install.
[[nodiscard]] bool DirectMode();

/// Direct mode's title (0 otherwise): the direct renderer keeps its caches per title
/// (renderer_vulkan.cpp, DM_TEMP).
[[nodiscard]] u64 DirectProgramId();

/// Direct mode, the emulator shutting down: the game's threads waiting on the
/// host inside a halted NVN call return, so the kernel can suspend them. Before the cores are
/// suspended (System::Pause, ShutdownMainProcess). Nothing otherwise.
void StopDirect();

/// Decides the boot's renderer (Choice) and, in direct mode, finds the NVN driver's
/// exports in the application's modules (after they are loaded). Logs the decision.
void Install(Core::System& system, Kernel::KProcess& process);

/// The Vulkan replay started successfully, before the guest runs.
void DirectStarted();

/// Direct mode, the Vulkan renderer being made (before the game runs): the direct renderer could
/// not start on the device. The boot falls back to GPU emulation, and so does every later boot
/// in this process that asks for the direct renderer. Logged.
void DirectFailed();

/// EDEN_NX2_NVN_STARTUP_TRACE=1: logs the process's memory at a startup phase of a direct boot.
void TraceStartup(std::string_view phase);

/// The boot is over (the end of System::ShutdownMainProcess): its hook state goes, and the direct
/// renderer's front end and replay return to their state before the first boot
/// (nvn_direct_reset).
void Uninstall();

/// The renderer Install chose for the current (or last) boot.
[[nodiscard]] RendererChoice Choice();

/// Whether the settings request NVN Direct for the next boot, before eligibility is known.
[[nodiscard]] bool DirectRequested();

/// This process was started by MainWindow::RelaunchForGame (LICHII_RELAUNCH_WAIT_PID): it does not
/// relaunch again (NeedsFreshProcess).
void MarkRelaunched();

/// The next boot, under the current settings, asks for the direct renderer, and an earlier boot's
/// direct renderer could not be reset: the boot would fall back (RendererChoice::NotReset) unless
/// a fresh process runs it (NeedsFreshProcess).
[[nodiscard]] bool DirectNeedsFreshProcess();

/// UserConfig::block_entry_hook (null unless the direct renderer is asked for).
Dynarmic::A64::BlockEntryHook (*BlockEntryHookFunction())(u64 pc);

/// InstructionCacheOperationRaised(BlockEntryHook, pc) from a core's JIT. Returns whether the
/// JIT must halt for a call that may block: it then runs from the kernel as SVC HaltedCallSvc.
[[nodiscard]] bool OnBlockEntry(u64 pc, Dynarmic::A64::Jit& jit, Core::Memory::Memory& memory,
                                Kernel::KProcess* process);

/// The SVC number a halted NVN call takes (no real SVC is above 0xffff).
constexpr u32 HaltedCallSvc = 0x10000;

/// Direct mode: runs the NVN call that halted the JIT on this host thread, from Svc::Call (the
/// kernel may wait or switch threads here). args: x0-x7, x0 the result.
void RunHaltedCall(Core::System& system, std::array<u64, 8>& args);

/// NVN2 direct mode: a guest write reached write-tracked pool memory [address, address + size)
/// (Core::Memory's rasterizer write path): the replay's textures over it are uploaded again.
void OnDirectCpuWrite(u64 address, u64 size);

/// The guest unmaps [address, address + size) of a page table (Core::Memory::UnmapRegion): in
/// direct mode the application's pool memory imported there is given back first, and the
/// replay's readers of guest memory wait until OnGuestUnmapDone. Returns whether the caller
/// must call OnGuestUnmapDone once the pages are unmapped.
bool OnGuestUnmap(const Common::PageTable& page_table, u64 address, u64 size);
void OnGuestUnmapDone();

} // namespace Core::NvnHook
