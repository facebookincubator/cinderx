// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <fmt/format.h>

#include <array>
#include <iosfwd>
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

#define FOREACH_XMM(X) \
  X(XMM0)              \
  X(XMM1)              \
  X(XMM2)              \
  X(XMM3)              \
  X(XMM4)              \
  X(XMM5)              \
  X(XMM6)              \
  X(XMM7)              \
  X(XMM8)              \
  X(XMM9)              \
  X(XMM10)             \
  X(XMM11)             \
  X(XMM12)             \
  X(XMM13)             \
  X(XMM14)             \
  X(XMM15)

enum class RegId : uint32_t {
#define DEFINE_REG(V, ...) V,
  FOREACH_GP(DEFINE_REG) FOREACH_XMM(DEFINE_REG)
#undef DEFINE_REG
};

constexpr uint32_t raw(RegId id) {
  return static_cast<uint32_t>(id);
}

#define COUNT_REGS(...) +1
constexpr int NUM_GP_REGS = FOREACH_GP(COUNT_REGS);
constexpr int XMM_REG_BASE = raw(RegId::XMM0);
constexpr int NUM_XMM_REGS = FOREACH_XMM(COUNT_REGS);
constexpr int NUM_REGS = NUM_GP_REGS + NUM_XMM_REGS;
#undef COUNT_REGS

constexpr std::string_view name(RegId id) {
  switch (id) {
#define STRING_REG(V, ...) \
  case RegId::V:           \
    return #V;

    FOREACH_GP(STRING_REG)
    FOREACH_XMM(STRING_REG)
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
struct PhyLocation {
  static constexpr int REG_INVALID = -1;

#define DEFINE_REG(V, ...) static constexpr int V = raw(RegId::V);
  FOREACH_GP(DEFINE_REG)
  FOREACH_XMM(DEFINE_REG)
#undef DEFINE_REG

  PhyLocation() = default;

  /* implicit */ constexpr PhyLocation(RegId reg, int size = 64)
      : PhyLocation{static_cast<int>(reg), size} {}

  /* implicit */ constexpr PhyLocation(int l, int s = 64)
      : loc(l), bitSize(s) {}

  bool is_memory() const {
    return loc < 0;
  }

  bool is_register() const {
    return loc >= 0;
  }

  bool is_gp_register() const {
    return is_register() && loc < XMM_REG_BASE;
  }

  bool is_fp_register() const {
    return is_register() && loc >= XMM_REG_BASE;
  }

  int loc{REG_INVALID};
  size_t bitSize{64};

  std::string toString() const;

  int getRegSize() const {
    // TODO: Hardcoding XMM size temporarily for correctness.
    return is_fp_register() ? 128 : bitSize;
  }

  bool operator==(const PhyLocation& rhs) const {
    return loc == rhs.loc;
  }
  bool operator==(int rhs) const {
    return loc == rhs;
  }

  bool operator!=(const PhyLocation& rhs) const {
    return loc != rhs.loc;
  }
  bool operator!=(int rhs) const {
    return loc != rhs;
  }

  // Parses the register name in string and returns the corresponding
  // physical register.
  // Returns REG_INVALID if name is not a valid register name.
  static PhyLocation parse(const std::string& name);
};

// Define global definitions like `RAX` and `XMM0`.
#define DEFINE_PHY_GP_REG(V64, V32, V16, V8) \
  constexpr PhyLocation V64{RegId::V64, 64}; \
  constexpr PhyLocation V32{RegId::V64, 32}; \
  constexpr PhyLocation V16{RegId::V64, 16}; \
  constexpr PhyLocation V8{RegId::V64, 8};

#define DEFINE_PHY_XMM_REG(V) constexpr PhyLocation V{RegId::V, 128};

FOREACH_GP(DEFINE_PHY_GP_REG)
FOREACH_XMM(DEFINE_PHY_XMM_REG)

#undef DEFINE_PHY_GP_REG
#undef DEFINE_PHY_XMM_REG

class PhyRegisterSet {
 public:
  constexpr PhyRegisterSet() : rs_(0) {}
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
    return rs_ == 0;
  }

  int count() const {
    return popcount(rs_);
  }

  PhyLocation GetFirst() const {
    return __builtin_ctz(rs_);
  }

  constexpr void RemoveFirst() {
    rs_ &= (rs_ - 1);
  }

  void Set(PhyLocation reg) {
    rs_ |= (1 << reg.loc);
  }
  void Reset(PhyLocation reg) {
    rs_ &= ~(1 << reg.loc);
  }
  void ResetAll() {
    rs_ = 0;
  }

  bool Has(PhyLocation reg) const {
    return rs_ & (1 << reg.loc);
  }

  constexpr int GetMask() const {
    return rs_;
  }

 private:
  unsigned rs_;
};

std::ostream& operator<<(std::ostream& out, const PhyLocation& loc);

#define ADD_REG(v, ...) | PhyLocation::v
constexpr PhyRegisterSet ALL_GP_REGISTERS =
    PhyRegisterSet() FOREACH_GP(ADD_REG);
constexpr PhyRegisterSet ALL_XMM_REGISTERS =
    PhyRegisterSet() FOREACH_XMM(ADD_REG);
constexpr PhyRegisterSet ALL_REGISTERS = ALL_GP_REGISTERS | ALL_XMM_REGISTERS;
#undef ADD_REG

constexpr PhyRegisterSet STACK_REGISTERS = PhyRegisterSet(RSP) | RBP;

constexpr PhyRegisterSet INIT_REGISTERS = ALL_REGISTERS - STACK_REGISTERS;

constexpr PhyRegisterSet CALLER_SAVE_REGS = PhyRegisterSet(RAX) | RCX | RDX |
    RSI | RDI | R8 | R9 | R10 | R11 | ALL_XMM_REGISTERS;

constexpr PhyRegisterSet CALLEE_SAVE_REGS = INIT_REGISTERS - CALLER_SAVE_REGS;

constexpr auto ARGUMENT_REGS = std::to_array({RDI, RSI, RDX, RCX, R8, R9});

constexpr auto RETURN_REGS = std::to_array({RAX, RDX});

constexpr auto FP_ARGUMENT_REGS =
    std::to_array({XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7});

// This is where the function prologue will initially store this data at entry
// to the function body. The register allocator may move things around from
// there.
constexpr PhyLocation INITIAL_EXTRA_ARGS_REG = R10;
constexpr PhyLocation INITIAL_TSTATE_REG = R11;
constexpr PhyLocation INITIAL_INTERPRETER_FRAME_REG = R12;
// This is often provided by the first argument in the vector call protocol.
constexpr PhyLocation INITIAL_FUNC_REG = ARGUMENT_REGS[0];

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
