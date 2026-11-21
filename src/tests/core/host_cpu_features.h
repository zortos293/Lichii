// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Tests {

/// FEAT_FP16 (scalar half-precision data processing), which the arm64 backend's scalar
/// half-precision emitters need: they assert without it. Asks the OS as oaknut's detection does
/// (hw.optional.arm.FEAT_FP16 on macOS, HWCAP_FPHP on Linux), which can't be included here as well
/// as in dynarmic. False on other hosts and other architectures.
bool HostHasScalarFP16();

} // namespace Tests
