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

#define FOREACH_GP(X) \
  X(X0, W0)           \
  X(X1, W1)           \
  X(X2, W2)           \
  X(X3, W3)           \
  X(X4, W4)           \
  X(X5, W5)           \
  X(X6, W6)           \
  X(X7, W7)           \
  X(X8, W8)           \
  X(X9, W9)           \
  X(X10, W10)         \
  X(X11, W11)         \
  X(X12, W12)         \
  X(X13, W13)         \
  X(X14, W14)         \
  X(X15, W15)         \
  X(X16, W16)         \
  X(X17, W17)         \
  X(X18, W18)         \
  X(X19, W19)         \
  X(X20, W20)         \
  X(X21, W21)         \
  X(X22, W22)         \
  X(X23, W23)         \
  X(X24, W24)         \
  X(X25, W25)         \
  X(X26, W26)         \
  X(X27, W27)         \
  X(X28, W28)         \
  X(X29, W29)         \
  X(X30, W30)         \
  X(XZR, WZR)

#define FOREACH_VECD(X) \
  X(D0)                 \
  X(D1)                 \
  X(D2)                 \
  X(D3)                 \
  X(D4)                 \
  X(D5)                 \
  X(D6)                 \
  X(D7)                 \
  X(D8)                 \
  X(D9)                 \
  X(D10)                \
  X(D11)                \
  X(D12)                \
  X(D13)                \
  X(D14)                \
  X(D15)                \
  X(D16)                \
  X(D17)                \
  X(D18)                \
  X(D19)                \
  X(D20)                \
  X(D21)                \
  X(D22)                \
  X(D23)                \
  X(D24)                \
  X(D25)                \
  X(D26)                \
  X(D27)                \
  X(D28)                \
  X(D29)                \
  X(D30)                \
  X(D31)

enum class RegId : uint32_t {
#define DEFINE_REG(V, ...) V,
  FOREACH_GP(DEFINE_REG) FOREACH_VECD(DEFINE_REG)
#undef DEFINE_REG
      SP = 0xFFFF,
};

constexpr uint32_t raw(RegId id) {
  return static_cast<uint32_t>(id);
}

#define COUNT_REGS(...) +1
constexpr int NUM_GP_REGS = FOREACH_GP(COUNT_REGS);
constexpr int VECD_REG_BASE = raw(RegId::D0);
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
    case RegId::SP:
      return "SP";
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized register ID {}", raw(id));
  }
}

constexpr std::string_view name32(RegId id) {
  switch (id) {
#define STRING_REG(V64, V32) \
  case RegId::V64:           \
    return #V32;

    FOREACH_GP(STRING_REG)
#undef STRING_REG
    default:
      JIT_ABORT("Unrecognized 32-bit register ID {}", raw(id));
  }
}

// A physical location (register or stack slot). If this represents a stack
// slot (is_memory() is true) then `loc` is relative to X29 (the frame pointer).
struct PhyLocation : PhyLocationBase<RegId, VECD_REG_BASE, NUM_REGS> {
  using Base = PhyLocationBase<RegId, VECD_REG_BASE, NUM_REGS>;
  using Base::Base;

#define DEFINE_REG(V, ...) static constexpr int V = raw(RegId::V);
  FOREACH_GP(DEFINE_REG)
  FOREACH_VECD(DEFINE_REG)
#undef DEFINE_REG
  static constexpr int SP = raw(RegId::SP);

  // Parse a register name and return the corresponding physical register.
  // Return REG_INVALID if the name is not a valid register name.  Does not
  // support parsing stack slots.
  static PhyLocation parse(std::string_view name);

  std::string toString() const;
};

// Define global definitions like `X0` and `D0`.
#define DEFINE_PHY_GP_REG(V64, V32)          \
  constexpr PhyLocation V64{RegId::V64, 64}; \
  constexpr PhyLocation V32{RegId::V64, 32};

#define DEFINE_PHY_VECD_REG(V) constexpr PhyLocation V{RegId::V, 64};

FOREACH_GP(DEFINE_PHY_GP_REG)
FOREACH_VECD(DEFINE_PHY_VECD_REG)
constexpr PhyLocation SP{RegId::SP, 64};

#undef DEFINE_PHY_GP_REG
#undef DEFINE_PHY_VECD_REG

using PhyRegisterSet = RegisterSet<PhyLocation, uint64_t>;

#define ADD_REG(v, ...) | PhyLocation::v
constexpr PhyRegisterSet ALL_GP_REGISTERS =
    PhyRegisterSet() FOREACH_GP(ADD_REG);
constexpr PhyRegisterSet ALL_VECD_REGISTERS =
    PhyRegisterSet() FOREACH_VECD(ADD_REG);
constexpr PhyRegisterSet ALL_REGISTERS = ALL_GP_REGISTERS | ALL_VECD_REGISTERS;
#undef ADD_REG

constexpr PhyRegisterSet DISALLOWED_REGISTERS = PhyRegisterSet(X29) /* FP */ |
    X30 /* LR */ | XZR /* zero */ | X13 /* scratch0 */ | X14 /* scratch1 */ |
    X16 /* IP0 */ | D16 /* fp_scratch0 */ | D17 /* fp_scratch1 */;

constexpr PhyRegisterSet INIT_REGISTERS = ALL_REGISTERS - DISALLOWED_REGISTERS;

constexpr PhyRegisterSet CALLEE_SAVE_REGS = PhyRegisterSet(X19) | X20 | X21 |
    X22 | X23 | X24 | X25 | X26 | X27 | X28 | D8 | D9 | D10 | D11 | D12 | D13 |
    D14 | D15;

constexpr PhyRegisterSet CALLER_SAVE_REGS = INIT_REGISTERS - CALLEE_SAVE_REGS;

constexpr auto ARGUMENT_REGS = std::to_array({X0, X1, X2, X3, X4, X5, X6, X7});

constexpr auto RETURN_REGS = std::to_array({X0, X1});

constexpr auto FP_ARGUMENT_REGS =
    std::to_array({D0, D1, D2, D3, D4, D5, D6, D7});

// This is where the function prologue will initially store this data at entry
// to the function body. The register allocator may move things around from
// there.
constexpr PhyLocation INITIAL_EXTRA_ARGS_REG = X10;
constexpr PhyLocation INITIAL_TSTATE_REG = X11;

// This is often provided by the first argument in the vector call protocol.
constexpr PhyLocation INITIAL_FUNC_REG = ARGUMENT_REGS[0];

constexpr int kShadowSpaceSize = 0;

} // namespace jit::codegen
