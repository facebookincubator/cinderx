// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/arch.h"

#if defined(CINDER_AARCH64)

using namespace asmjit;

namespace jit::codegen::arch {

// Attempt to build a pointer using an offset from a base register. If it is
// not possible to do so, return std::nullopt.
static std::optional<a64::Mem>
ptr_offset_try(const a64::Gp& base, int32_t offset, AccessSize access_size) {
  if (offset >= -256 && offset < 256) {
    // Unscaled immediate offset
    return a64::ptr(base, offset);
  }

  if (offset >= 0 && (offset & (static_cast<int32_t>(access_size) - 1)) == 0 &&
      offset < static_cast<int32_t>(access_size) * 4096) {
    // Unsigned scaled immediate
    return a64::ptr(base, offset);
  }

  return std::nullopt;
}

// Build a pointer using an offset from a base register. If it is not possible
// to do so, terminate the program. This should only be used in contexts where
// the offset is known to be within range or it is not possible to access a
// builder.
a64::Mem
ptr_offset(const a64::Gp& base, int32_t offset, AccessSize access_size) {
  auto opt = ptr_offset_try(base, offset, access_size);
  JIT_CHECK(opt.has_value(), "offset out of range");
  return opt.value();
}

// a64 has a couple of addressing modes, and we want to use the one with the
// fewest instructions possible. In the best case, this will no instructions
// because we can address it directly. Alternatively we could add/sub an offset
// into a scratch register and address that. Finally, in the worst case this
// will be 4 movz/movk instructions, then an indirect pointer through a
// register.
a64::Mem ptr_resolve(
    a64::Builder* as,
    const a64::Gp& base,
    int32_t offset,
    const a64::Gp& scratch,
    AccessSize access_size) {
  if (auto opt = ptr_offset_try(base, offset, access_size)) {
    return opt.value();
  }

  if (offset >= 0 && arm::Utils::isAddSubImm(static_cast<uint64_t>(offset))) {
    // Add immediate
    as->add(scratch, base, offset);
    return a64::ptr(scratch);
  }
  if (offset < 0 && arm::Utils::isAddSubImm(static_cast<uint64_t>(-offset))) {
    // Sub immediate
    as->sub(scratch, base, -offset);
    return a64::ptr(scratch);
  }

  // Base + index
  as->mov(scratch, offset);
  return a64::ptr(base, scratch);
}

void cmp_immediate(a64::Builder* as, const arch::Gp& reg, uint64_t imm) {
  if (arm::Utils::isAddSubImm(imm)) {
    as->cmp(reg, imm);
  } else if (arm::Utils::isAddSubImm(-imm)) {
    as->cmn(reg, -imm);
  } else {
    as->mov(arch::reg_scratch_0, imm);
    as->cmp(reg, arch::reg_scratch_0);
  }
}

void add_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    uint64_t rhsi) {
  if (rhsi == 0) {
    if (res != lhs) {
      as->mov(res, lhs);
    }
  } else if (arm::Utils::isAddSubImm(rhsi)) {
    as->add(res, lhs, rhsi);
  } else {
    as->mov(arch::reg_scratch_0, rhsi);
    as->add(res, lhs, arch::reg_scratch_0);
  }
}

void sub_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    uint64_t rhsi) {
  if (rhsi == 0) {
    if (res != lhs) {
      as->mov(res, lhs);
    }
  } else if (arm::Utils::isAddSubImm(rhsi)) {
    as->sub(res, lhs, rhsi);
  } else {
    as->mov(arch::reg_scratch_0, rhsi);
    as->sub(res, lhs, arch::reg_scratch_0);
  }
}

void add_signed_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    int64_t rhsi) {
  uint64_t rshu = static_cast<uint64_t>(rhsi);
  if (rhsi >= 0) {
    add_immediate(as, res, lhs, rshu);
  } else {
    sub_immediate(as, res, lhs, -rshu);
  }
}

} // namespace jit::codegen::arch

#endif

namespace jit::codegen {

std::ostream& operator<<(std::ostream& out, const PhyLocation& loc) {
  return out << loc.toString();
}

} // namespace jit::codegen
