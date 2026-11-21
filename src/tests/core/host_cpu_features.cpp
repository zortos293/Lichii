// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tests/core/host_cpu_features.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#elif defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
#endif

namespace Tests {

bool HostHasScalarFP16() {
#if defined(__APPLE__) && defined(__aarch64__)
    static const bool has_fp16 = [] {
        int value = 0;
        size_t size = sizeof(value);
        return sysctlbyname("hw.optional.arm.FEAT_FP16", &value, &size, nullptr, 0) == 0 &&
               value != 0;
    }();
    return has_fp16;
#elif defined(__linux__) && defined(__aarch64__)
    // HWCAP_FPHP is bit 9; bit 10 is HWCAP_ASIMDHP, the vector forms.
    static const bool has_fp16 = (getauxval(AT_HWCAP) & (1UL << 9)) != 0;
    return has_fp16;
#else
    return false;
#endif
}

} // namespace Tests
