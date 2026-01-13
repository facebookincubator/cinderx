// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Jit/codegen/arch/detection.h"
#include "fmt/ostream.h"

#include <asmjit/asmjit.h>

#include <iosfwd>

#if defined(CINDER_X86_64)

#include "cinderx/Jit/codegen/arch/x86_64.h"

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

constexpr auto reg_scratch_0_loc = RAX;

constexpr auto reg_general_return_loc = RAX;
constexpr auto reg_general_auxilary_return_loc = RDX;
constexpr auto reg_double_return_loc = XMM0;
constexpr auto reg_double_auxilary_return_loc = XMM1;
constexpr auto reg_frame_pointer_loc = RBP;
constexpr auto reg_stack_pointer_loc = RSP;

} // namespace jit::codegen::arch

#else

#include "cinderx/Jit/codegen/arch/unknown.h"

namespace jit::codegen::arch {

class Builder : public asmjit::BaseBuilder {
 public:
  explicit Builder(asmjit::CodeHolder* code = nullptr) noexcept {
    if (code) {
      code->attach(this);
    }
  }
};

using Emitter = asmjit::BaseEmitter;
using Gp = asmjit::BaseReg;
using Mem = asmjit::BaseMem;
using Reg = asmjit::BaseReg;
using VecD = asmjit::BaseReg;

template <typename T>
struct EmitterExplicitT {};

constexpr auto reg_scratch_deopt = asmjit::BaseReg();

constexpr auto reg_scratch_0_loc = R3;

constexpr auto reg_general_return_loc = R0;
constexpr auto reg_general_auxilary_return_loc = R1;
constexpr auto reg_double_return_loc = D0;
constexpr auto reg_double_auxilary_return_loc = D1;
constexpr auto reg_frame_pointer_loc = R3;
constexpr auto reg_stack_pointer_loc = SP;

} // namespace jit::codegen::arch

#endif

namespace jit::codegen {

std::ostream& operator<<(std::ostream& out, const PhyLocation& loc);

} // namespace jit::codegen

inline auto format_as(jit::codegen::RegId reg) {
  return fmt::underlying(reg);
}

namespace std {

template <>
struct hash<jit::codegen::PhyLocation> {
  std::size_t operator()(jit::codegen::PhyLocation const& s) const noexcept {
    return s.loc;
  }
};

} // namespace std

template <>
struct fmt::formatter<jit::codegen::PhyLocation> : fmt::ostream_formatter {};
