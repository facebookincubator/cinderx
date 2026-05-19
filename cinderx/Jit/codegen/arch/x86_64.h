// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch/phy_location.h"
#include "cinderx/Jit/codegen/arch/register_set.h"

#include <fmt/format.h>

#include <array>
#include <string>
#include <string_view>

namespace jit::codegen {

#define FOREACH_GP(X)      \
  X(RAX, EAX, AX, AL)      \
  X(RCX, ECX, CX, CL)      \
  X(RDX, EDX, DX, DL)      \
  X(RBX, EBX, BX, BL)      \
  X(RSP, ESP, SP, SPL)     \
  X(RBP, EBP, BP, BPL)     \
  X(RSI, ESI, SI, SIL)     \
  X(RDI, EDI, DI, DIL)     \
  X(R8, R8D, R8W, R8B)     \
  X(R9, R9D, R9W, R9B)     \
  X(R10, R10D, R10W, R10B) \
  X(R11, R11D, R11W, R11B) \
  X(R12, R12D, R12W, R12B) \
  X(R13, R13D, R13W, R13B) \
  X(R14, R14D, R14W, R14B) \
  X(R15, R15D, R15W, R15B)

#define FOREACH_VECD(X) \
  X(XMM0)               \
  X(XMM1)               \
  X(XMM2)               \
  X(XMM3)               \
  X(XMM4)               \
  X(XMM5)               \
  X(XMM6)               \
  X(XMM7)               \
  X(XMM8)               \
  X(XMM9)               \
  X(XMM10)              \
  X(XMM11)              \
  X(XMM12)              \
  X(XMM13)              \
  X(XMM14)              \
  X(XMM15)

enum class RegId : uint32_t {
#define DEFINE_REG(V, ...) V,
  FOREACH_GP(DEFINE_REG) FOREACH_VECD(DEFINE_REG)
#undef DEFINE_REG
};

constexpr uint32_t raw(RegId id) {
  return static_cast<uint32_t>(id);
}

#define COUNT_REGS(...) +1
constexpr int NUM_GP_REGS = FOREACH_GP(COUNT_REGS);
constexpr int VECD_REG_BASE = raw(RegId::XMM0);
constexpr int NUM_VECD_REGS = FOREACH_VECD(COUNT_REGS);
constexpr int NUM_REGS = NUM_GP_REGS + NUM_VECD_REGS;
#undef COUNT_REGS

constexpr std::string_view name(RegId id) {
  switch (id) {
#define STRING_REG(V, ...) \
  case RegId::V:           \
    return #V;

    FOREACH_GP(STRING_REG)
    FOREACH_VECD(STRING_REG)
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized register ID {}", raw(id));
  }
}

constexpr std::string_view name32(RegId id) {
  switch (id) {
#define STRING_REG(V64, V, ...) \
  case RegId::V64:              \
    return #V;

    FOREACH_GP(STRING_REG)
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized 32-bit register ID {}", raw(id));
  }
}

constexpr std::string_view name16(RegId id) {
  switch (id) {
#define STRING_REG(V64, V32, V, ...) \
  case RegId::V64:                   \
    return #V;

    FOREACH_GP(STRING_REG)
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized 16-bit register ID {}", raw(id));
  }
}

constexpr std::string_view name8(RegId id) {
  switch (id) {
#define STRING_REG(V64, V32, V16, V, ...) \
  case RegId::V64:                        \
    return #V;

    FOREACH_GP(STRING_REG)
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized 8-bit register ID {}", raw(id));
  }
}

// A physical location (register or stack slot). If this represents a stack
// slot (is_memory() is true) then `loc` is relative to RBP.
struct PhyLocation : PhyLocationBase<RegId, VECD_REG_BASE, NUM_REGS> {
  using Base = PhyLocationBase<RegId, VECD_REG_BASE, NUM_REGS>;
  using Base::Base;

#define DEFINE_REG(V, ...) static constexpr int V = raw(RegId::V);
  FOREACH_GP(DEFINE_REG)
  FOREACH_VECD(DEFINE_REG)
#undef DEFINE_REG

  // Parse a register name and return the corresponding physical register.
  // Return REG_INVALID if the name is not a valid register name.  Does not
  // support parsing stack slots.
  static PhyLocation parse(std::string_view name);

  std::string toString() const;
};

// Define global definitions like `RAX` and `XMM0`.
#define DEFINE_PHY_GP_REG(V64, V32, V16, V8) \
  constexpr PhyLocation V64{RegId::V64, 64}; \
  constexpr PhyLocation V32{RegId::V64, 32}; \
  constexpr PhyLocation V16{RegId::V64, 16}; \
  constexpr PhyLocation V8{RegId::V64, 8};

#define DEFINE_PHY_VECD_REG(V) constexpr PhyLocation V{RegId::V, 128};

FOREACH_GP(DEFINE_PHY_GP_REG)
FOREACH_VECD(DEFINE_PHY_VECD_REG)

#undef DEFINE_PHY_GP_REG
#undef DEFINE_PHY_VECD_REG

using PhyRegisterSet = RegisterSet<PhyLocation, unsigned>;

#define ADD_REG(v, ...) | PhyLocation::v
constexpr PhyRegisterSet ALL_GP_REGISTERS =
    PhyRegisterSet() FOREACH_GP(ADD_REG);
constexpr PhyRegisterSet ALL_VECD_REGISTERS =
    PhyRegisterSet() FOREACH_VECD(ADD_REG);
constexpr PhyRegisterSet ALL_REGISTERS = ALL_GP_REGISTERS | ALL_VECD_REGISTERS;
#undef ADD_REG

constexpr PhyRegisterSet DISALLOWED_REGISTERS = PhyRegisterSet(RSP) | RBP;

constexpr PhyRegisterSet INIT_REGISTERS = ALL_REGISTERS - DISALLOWED_REGISTERS;

#ifdef _WIN32
// Windows x64: RDI and RSI are callee-saved; XMM6-XMM15 are callee-saved.
constexpr PhyRegisterSet CALLER_SAVE_REGS = PhyRegisterSet(RAX) | RCX | RDX |
    R8 | R9 | R10 | R11 | PhyRegisterSet(XMM0) | XMM1 | XMM2 | XMM3 | XMM4 |
    XMM5;
#else
constexpr auto CALLER_SAVE_GP_REGS =
    std::to_array({RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11});

constexpr PhyRegisterSet CALLER_SAVE_REGS =
    PhyRegisterSet(CALLER_SAVE_GP_REGS) | ALL_VECD_REGISTERS;
#endif

constexpr PhyRegisterSet CALLEE_SAVE_REGS = INIT_REGISTERS - CALLER_SAVE_REGS;

#ifdef _WIN32
constexpr auto ARGUMENT_REGS = std::to_array({RCX, RDX, R8, R9});
#else
constexpr auto ARGUMENT_REGS = std::to_array({RDI, RSI, RDX, RCX, R8, R9});
#endif

constexpr auto RETURN_REGS = std::to_array({RAX, RDX});

#ifdef _WIN32
constexpr auto FP_ARGUMENT_REGS = std::to_array({XMM0, XMM1, XMM2, XMM3});
#else
constexpr auto FP_ARGUMENT_REGS =
    std::to_array({XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7});
#endif

// This is where the function prologue will initially store this data at entry
// to the function body. The register allocator may move things around from
// there.
constexpr PhyLocation INITIAL_EXTRA_ARGS_REG = R10;
constexpr PhyLocation INITIAL_TSTATE_REG = R11;
// This is often provided by the first argument in the vector call protocol.
constexpr PhyLocation INITIAL_FUNC_REG = ARGUMENT_REGS[0];

// Shadow space (home space) required by the Windows x64 calling convention.
// The caller must always reserve 32 bytes above the return address for the
// callee to spill register arguments.  System V does not use shadow space.
#ifdef _WIN32
constexpr int kShadowSpaceSize = 32;
#else
constexpr int kShadowSpaceSize = 0;
#endif

} // namespace jit::codegen
