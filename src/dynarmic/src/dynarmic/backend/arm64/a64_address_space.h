// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Arm64 {

struct A64JitState;
struct EmittedBlockInfo;

class A64AddressSpace final : public AddressSpace {
public:
    /// The dispatcher calls `lookup_block` with the running Jit's A64JitState::jit_impl. The
    /// callbacks of every Jit that runs this code must have `conf.callbacks`' dynamic type.
    using LookupBlock = CodePtr (*)(void* jit_impl, A64JitState& jit_state);
    A64AddressSpace(const A64::UserConfig& conf, LookupBlock lookup_block);

    void GenerateIR(IR::Block& ir_block, IR::LocationDescriptor) const override;

    /// Whether blocks were dropped.
    bool InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

    void ClearCache() override;

    using AddressSpace::PrepareSpace;
    EmittedBlockInfo EmitBlock(IR::Block block) {
        return Emit(std::move(block));
    }
    HaltReason RunCode(CodePtr entry_point, A64JitState& jit_state, volatile u32* halt_reason) {
        return prelude_info.run_code(entry_point, &jit_state, halt_reason);
    }
    HaltReason StepCode(CodePtr entry_point, A64JitState& jit_state, volatile u32* halt_reason) {
        return prelude_info.step_code(entry_point, &jit_state, halt_reason);
    }
    /// A block entry that leaves run_code at once.
    CodePtr ReturnFromRunCode() const {
        return static_cast<CodePtr>(prelude_info.return_from_run_code);
    }
    /// The fastmem fault handler. `wrapper` replaces it and calls HandleFastmemFault itself.
    void SetFastmemCallback(std::function<FakeCall(u64 host_pc)> wrapper) {
        exception_handler.SetFastmemCallback(std::move(wrapper));
    }
    FakeCall HandleFastmemFault(u64 host_pc) {
        return FastmemCallback(host_pc);
    }

protected:
    void EmitPrelude(PreludeInfo& info);
    void EmitRegionPrelude(PreludeInfo& info) override {
        EmitPrelude(info);
    }
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;

    const A64::UserConfig conf;
    const LookupBlock lookup_block;
    BlockRangeInformation<u64> block_ranges;
};

}  // namespace Dynarmic::Backend::Arm64
