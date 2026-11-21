/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/callback.h"

#include "dynarmic/backend/x64/block_of_code.h"

namespace Dynarmic::Backend::X64 {

Callback::~Callback() = default;

void SimpleCallback::EmitCall(BlockOfCode& code, std::function<void(RegList)> l) const {
    l({code.ABI_PARAM1, code.ABI_PARAM2, code.ABI_PARAM3, code.ABI_PARAM4});
    code.CallFunction(fn);
}

void SimpleCallback::EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> l) const {
    l(code.ABI_PARAM1, {code.ABI_PARAM2, code.ABI_PARAM3, code.ABI_PARAM4});
    code.CallFunction(fn);
}

void ArgCallback::EmitCall(BlockOfCode& code, std::function<void(RegList)> l) const {
    l({code.ABI_PARAM2, code.ABI_PARAM3, code.ABI_PARAM4});
    if (arg_jit_offset) {
        code.mov(code.ABI_PARAM1, code.qword[code.ABI_JIT_PTR + *arg_jit_offset]);
    } else {
        code.mov(code.ABI_PARAM1, arg);
    }
    code.CallFunction(fn);
}

void ArgCallback::EmitCallWithReturnPointer(BlockOfCode& code, std::function<void(Xbyak::Reg64, RegList)> l) const {
#if defined(WIN32) && !defined(__MINGW64__)
    l(code.ABI_PARAM2, {code.ABI_PARAM3, code.ABI_PARAM4});
    const Xbyak::Reg64 arg_reg = code.ABI_PARAM1;
#else
    l(code.ABI_PARAM1, {code.ABI_PARAM3, code.ABI_PARAM4});
    const Xbyak::Reg64 arg_reg = code.ABI_PARAM2;
#endif
    if (arg_jit_offset) {
        code.mov(arg_reg, code.qword[code.ABI_JIT_PTR + *arg_jit_offset]);
    } else {
        code.mov(arg_reg, arg);
    }
    code.CallFunction(fn);
}

}  // namespace Dynarmic::Backend::X64
