// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <fmt/format.h>

#include <array>
#include <bit>
#include <string>
#include <string_view>

namespace jit::codegen {

#define FOREACH_GP(X) \
  X(R0)               \
  X(R1)               \
  X(R2)               \
  X(R3)

#define FOREACH_VECD(X) \
  X(D0)                 \
  X(D1)                 \
  X(D2)                 \
  X(D3)

enum class RegId : uint32_t {
#define DEFINE_REG(V) V,
  FOREACH_GP(DEFINE_REG) FOREACH_VECD(DEFINE_REG)
#undef DEFINE_REG
      SP,
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
#define STRING_REG(V) \
  case RegId::V:      \
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

// A physical location (register or stack slot). If this represents a stack
// slot (is_memory() is true) then `loc` is relative to R3.
struct PhyLocation {
  static constexpr int REG_INVALID = -1;

#define DEFINE_REG(V) static constexpr int V = raw(RegId::V);
  FOREACH_GP(DEFINE_REG)
  FOREACH_VECD(DEFINE_REG)
  static constexpr int SP = raw(RegId::SP);
#undef DEFINE_REG

  // Parse a register name and return the corresponding physical register.
  // Return REG_INVALID if the name is not a valid register name.  Does not
  // support parsing stack slots.
  static PhyLocation parse(std::string_view name);

  int32_t loc{REG_INVALID};
  uint32_t bitSize{64};

  PhyLocation() = default;

  /* implicit */ constexpr PhyLocation(RegId reg, size_t size = 64)
      : PhyLocation{static_cast<int>(reg), size} {}

  /* implicit */ constexpr PhyLocation(RegId reg, int size)
      : PhyLocation{static_cast<int>(reg), static_cast<size_t>(size)} {}

  /* implicit */ constexpr PhyLocation(int loc, size_t size = 64)
      : loc{loc}, bitSize{static_cast<uint32_t>(size)} {}

  /* implicit */ constexpr PhyLocation(int loc, int size)
      : PhyLocation{loc, static_cast<size_t>(size)} {}

  bool is_memory() const {
    return loc < 0;
  }

  bool is_register() const {
    return loc >= 0;
  }

  bool is_gp_register() const {
    return is_register() && loc < VECD_REG_BASE;
  }

  bool is_fp_register() const {
    return is_register() && loc >= VECD_REG_BASE;
  }

  std::string toString() const;

  bool operator==(const PhyLocation& rhs) const {
    return loc == rhs.loc;
  }

  bool operator!=(const PhyLocation& rhs) const {
    return loc != rhs.loc;
  }
};

// Define global definitions like `R0` and `D0`.
#define DEFINE_PHY_REG(V) constexpr PhyLocation V{RegId::V, 64};

FOREACH_GP(DEFINE_PHY_REG)
FOREACH_VECD(DEFINE_PHY_REG)
constexpr PhyLocation SP{RegId::SP, 64};

#undef DEFINE_PHY_GP_REG
#undef DEFINE_PHY_VECD_REG

class PhyRegisterSet {
 public:
  constexpr PhyRegisterSet() = default;
  explicit constexpr PhyRegisterSet(PhyLocation r) : rs_(0) {
    rs_ |= (1 << r.loc);
  }

  constexpr PhyRegisterSet operator|(PhyLocation reg) const {
    PhyRegisterSet set;
    set.rs_ = rs_ | (1 << reg.loc);
    return set;
  }

  constexpr PhyRegisterSet operator|(const PhyRegisterSet& rs) const {
    PhyRegisterSet res;
    res.rs_ = rs_ | rs.rs_;
    return res;
  }

  PhyRegisterSet& operator|=(const PhyRegisterSet& rs) {
    rs_ |= rs.rs_;
    return *this;
  }

  constexpr PhyRegisterSet operator-(PhyLocation rs) const {
    return operator-(PhyRegisterSet(rs));
  }

  constexpr PhyRegisterSet operator-(PhyRegisterSet rs) const {
    PhyRegisterSet set;
    set.rs_ = rs_ & ~(rs.rs_);
    return set;
  }

  constexpr PhyRegisterSet operator&(PhyRegisterSet rs) const {
    PhyRegisterSet set;
    set.rs_ = rs_ & rs.rs_;
    return set;
  }

  constexpr bool operator==(const PhyRegisterSet& rs) const {
    return rs_ == rs.rs_;
  }

  constexpr bool Empty() const {
    return rs_ == 0ULL;
  }

  constexpr int count() const {
    return std::popcount(rs_);
  }

  constexpr PhyLocation GetFirst() const {
    return std::countr_zero(rs_);
  }

  constexpr PhyLocation GetLast() const {
    return GetLastBit();
  }

  constexpr void RemoveFirst() {
    rs_ &= (rs_ - 1);
  }

  constexpr void RemoveLast() {
    rs_ &= ~(1U << GetLastBit());
  }

  constexpr void Set(PhyLocation reg) {
    rs_ |= (1 << reg.loc);
  }

  constexpr void Reset(PhyLocation reg) {
    rs_ &= ~(1 << reg.loc);
  }

  constexpr void ResetAll() {
    rs_ = 0;
  }

  constexpr bool Has(PhyLocation reg) const {
    return rs_ & (1 << reg.loc);
  }

 private:
  uint32_t rs_{0};

  constexpr int GetLastBit() const {
    return (sizeof(rs_) * CHAR_BIT - 1) - std::countl_zero(rs_);
  }
};

#define ADD_REG(V) | PhyLocation::V
constexpr PhyRegisterSet ALL_GP_REGISTERS =
    PhyRegisterSet() FOREACH_GP(ADD_REG);
constexpr PhyRegisterSet ALL_VECD_REGISTERS =
    PhyRegisterSet() FOREACH_VECD(ADD_REG);
constexpr PhyRegisterSet ALL_REGISTERS = ALL_GP_REGISTERS | ALL_VECD_REGISTERS;
#undef ADD_REG

constexpr PhyRegisterSet DISALLOWED_REGISTERS = PhyRegisterSet();
constexpr PhyRegisterSet INIT_REGISTERS = ALL_REGISTERS - DISALLOWED_REGISTERS;
constexpr PhyRegisterSet CALLEE_SAVE_REGS = PhyRegisterSet();
constexpr PhyRegisterSet CALLER_SAVE_REGS = INIT_REGISTERS - CALLEE_SAVE_REGS;

constexpr auto ARGUMENT_REGS = std::to_array({R0});
constexpr auto RETURN_REGS = std::to_array({R0});
constexpr auto FP_ARGUMENT_REGS = std::to_array({D0});

// This is where the function prologue will initially store this data at entry
// to the function body. The register allocator may move things around from
// there.
constexpr PhyLocation INITIAL_EXTRA_ARGS_REG = R1;
constexpr PhyLocation INITIAL_TSTATE_REG = R2;
// This is often provided by the first argument in the vector call protocol.
constexpr PhyLocation INITIAL_FUNC_REG = ARGUMENT_REGS[0];

} // namespace jit::codegen
