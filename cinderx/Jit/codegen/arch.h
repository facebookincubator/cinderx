// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <asmjit/asmjit.h>

#if defined(__x86_64__)
#define CINDER_X86_64

#include <asmjit/x86/x86builder.h>
#include <asmjit/x86/x86emitter.h>
#include <asmjit/x86/x86operand.h>

namespace jit::codegen::arch {

using Builder = asmjit::x86::Builder;
using Emitter = asmjit::x86::Emitter;
using Gp = asmjit::x86::Gp;
using Mem = asmjit::x86::Mem;
using Reg = asmjit::x86::Reg;
using VecD = asmjit::x86::Xmm;

template <typename T>
using EmitterExplicitT = asmjit::x86::EmitterExplicitT<T>;

// If you change this register you'll also need to change the deopt
// trampoline code that saves all registers.
constexpr auto reg_scratch_deopt = asmjit::x86::r15;
constexpr auto reg_general_return_64 = asmjit::x86::rax;

} // namespace jit::codegen::arch

#else
// This macro is a marker for places that need platform-specific code.
#define CINDER_UNSUPPORTED

namespace jit::codegen::arch {

class Builder : public asmjit::BaseBuilder {
 public:
  explicit Builder(asmjit::CodeHolder* code = nullptr) noexcept;
};

using Emitter = asmjit::BaseEmitter;
using Gp = asmjit::BaseReg;
using Mem = asmjit::BaseMem;
using Reg = asmjit::BaseReg;
using VecD = asmjit::BaseReg;

template <typename T>
struct EmitterExplicitT {};

constexpr auto reg_scratch_deopt = asmjit::BaseReg();
constexpr auto reg_general_return_64 = asmjit::BaseReg();

} // namespace jit::codegen::arch

#endif
