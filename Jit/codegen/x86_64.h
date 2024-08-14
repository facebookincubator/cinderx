// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <array>
#include <iosfwd>
#include <set>
#include <string>
#include <unordered_map>

namespace jit::codegen {

// A physical location (register or stack slot). If this represents a stack
// slot (is_memory() is true) then `loc` is relative to RBP.
struct PhyLocation {
  PhyLocation() : loc(REG_INVALID), bitSize(64) {}
  constexpr PhyLocation(int l, int s = 64) : loc(l), bitSize(s) {}

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

  int loc;
  size_t bitSize;

#define FOREACH_GP64(X) \
  X(RAX)                \
  X(RCX)                \
  X(RDX)                \
  X(RBX)                \
  X(RSP)                \
  X(RBP)                \
  X(RSI)                \
  X(RDI)                \
  X(R8)                 \
  X(R9)                 \
  X(R10)                \
  X(R11)                \
  X(R12)                \
  X(R13)                \
  X(R14)                \
  X(R15)

#define FOREACH_GP32(X) \
  X(EAX)                \
  X(ECX)                \
  X(EDX)                \
  X(EBX)                \
  X(ESP)                \
  X(EBP)                \
  X(ESI)                \
  X(EDI)                \
  X(R8D)                \
  X(R9D)                \
  X(R10D)               \
  X(R11D)               \
  X(R12D)               \
  X(R13D)               \
  X(R14D)               \
  X(R15D)

#define FOREACH_GP16(X) \
  X(AX)                 \
  X(CX)                 \
  X(DX)                 \
  X(BX)                 \
  X(SP)                 \
  X(BP)                 \
  X(SI)                 \
  X(DI)                 \
  X(R8W)                \
  X(R9W)                \
  X(R10W)               \
  X(R11W)               \
  X(R12W)               \
  X(R13W)               \
  X(R14W)               \
  X(R15W)

#define FOREACH_GP8(X) \
  X(AL)                \
  X(CL)                \
  X(DL)                \
  X(BL)                \
  X(SPL)               \
  X(BPL)               \
  X(SIL)               \
  X(DIL)               \
  X(R8B)               \
  X(R9B)               \
  X(R10B)              \
  X(R11B)              \
  X(R12B)              \
  X(R13B)              \
  X(R14B)              \
  X(R15B)

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

  enum Reg : int {
    REG_INVALID = -1,
#define DECLARE_REG(v, ...) v,
    FOREACH_GP64(DECLARE_REG) FOREACH_XMM(DECLARE_REG)
#undef DECLARE_REG
  };

#define COUNT_REGS(...) +1
  static constexpr int NUM_GP_REGS = FOREACH_GP64(COUNT_REGS);
  static constexpr int XMM_REG_BASE = XMM0;
  static constexpr int NUM_XMM_REGS = FOREACH_XMM(COUNT_REGS);
  static constexpr int NUM_REGS = NUM_GP_REGS + NUM_XMM_REGS;
#undef COUNT_REGS

  static const char* regName(Reg reg) {
    JIT_CHECK(reg >= 0, "reg must be nonnegative");
    switch (reg) {
#define DECLARE_REG(v, ...) \
  case v:                   \
    return #v;
      FOREACH_GP64(DECLARE_REG)
      FOREACH_XMM(DECLARE_REG)
#undef DECLARE_REG
      case REG_INVALID:
        JIT_ABORT("Invalid register");
    }
    JIT_ABORT("Unknown register {}", reg);
  }

  static const char* reg32Name(Reg reg) {
    constexpr std::array<const char*, NUM_GP_REGS> gp32_names = {
#define TO_STR(v) #v,
        FOREACH_GP32(TO_STR)
#undef TO_STR
    };
    JIT_CHECK(reg >= 0 && reg < gp32_names.size(), "Bad register ID: {}", reg);
    return gp32_names[reg];
  }

  static const char* reg16Name(Reg reg) {
    constexpr std::array<const char*, NUM_GP_REGS> gp16_names = {
#define TO_STR(v) #v,
        FOREACH_GP16(TO_STR)
#undef TO_STR
    };
    JIT_CHECK(reg >= 0 && reg < gp16_names.size(), "Bad register ID: {}", reg);
    return gp16_names[reg];
  }

  static const char* reg8Name(Reg reg) {
    constexpr std::array<const char*, NUM_GP_REGS> gp8_names = {
#define TO_STR(v) #v,
        FOREACH_GP8(TO_STR)
#undef TO_STR
    };
    JIT_CHECK(reg >= 0 && reg < gp8_names.size(), "Bad register ID: {}", reg);
    return gp8_names[reg];
  }

  std::string toString() const {
    if (is_memory()) {
      return fmt::format("[RBP{}]", loc);
    } else if (bitSize == 32) {
      return reg32Name(static_cast<Reg>(loc));
    } else if (bitSize == 16) {
      return reg16Name(static_cast<Reg>(loc));
    } else if (bitSize == 8) {
      return reg8Name(static_cast<Reg>(loc));
    } else {
      return regName(static_cast<Reg>(loc));
    }
  }

  int getRegSize() const {
    // TODO: Hardcoding XMM size temporarily for correctness.
    return (loc >= XMM0 && loc <= XMM15) ? 128 : bitSize;
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
static constexpr PhyRegisterSet ALL_GP_REGISTERS =
    PhyRegisterSet() FOREACH_GP64(ADD_REG);
static constexpr PhyRegisterSet ALL_XMM_REGISTERS =
    PhyRegisterSet() FOREACH_XMM(ADD_REG);
static constexpr PhyRegisterSet ALL_REGISTERS =
    ALL_GP_REGISTERS | ALL_XMM_REGISTERS;
#undef ADD_REG

static constexpr PhyRegisterSet STACK_REGISTERS =
    PhyRegisterSet(PhyLocation::RSP) | PhyLocation::RBP;

static constexpr PhyRegisterSet INIT_REGISTERS =
    ALL_REGISTERS - STACK_REGISTERS;

static constexpr PhyRegisterSet CALLER_SAVE_REGS =
    PhyRegisterSet(PhyLocation::RAX) | PhyLocation::RCX | PhyLocation::RDX |
    PhyLocation::RSI | PhyLocation::RDI | PhyLocation::R8 | PhyLocation::R9 |
    PhyLocation::R10 | PhyLocation::R11 | ALL_XMM_REGISTERS;

static constexpr PhyRegisterSet CALLEE_SAVE_REGS =
    INIT_REGISTERS - CALLER_SAVE_REGS;

constexpr auto ARGUMENT_REGS = std::to_array({
    PhyLocation::RDI,
    PhyLocation::RSI,
    PhyLocation::RDX,
    PhyLocation::RCX,
    PhyLocation::R8,
    PhyLocation::R9,
});

constexpr auto RETURN_REGS = std::to_array({
    PhyLocation::RAX,
    PhyLocation::RDX,
});

constexpr auto FP_ARGUMENT_REGS = std::to_array({
    PhyLocation::XMM0,
    PhyLocation::XMM1,
    PhyLocation::XMM2,
    PhyLocation::XMM3,
    PhyLocation::XMM4,
    PhyLocation::XMM5,
    PhyLocation::XMM6,
    PhyLocation::XMM7,
});

// This is where the function prologue will initially store this data at entry
// to the function body. The register allocator may move things around from
// there.
constexpr PhyLocation INITIAL_EXTRA_ARGS_REG = PhyLocation::R10;
constexpr PhyLocation INITIAL_TSTATE_REG = PhyLocation::R11;
// This is often provided by the first argument in the vector call protocol.
constexpr PhyLocation INITIAL_FUNC_REG = ARGUMENT_REGS[0];

} // namespace jit::codegen

namespace std {
template <>
struct hash<jit::codegen::PhyLocation> {
  std::size_t operator()(jit::codegen::PhyLocation const& s) const noexcept {
    return s.loc;
  }
};
} // namespace std
