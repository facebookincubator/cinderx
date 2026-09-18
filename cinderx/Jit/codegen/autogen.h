// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"

namespace cinderx::jit::codegen::autogen {

// A machine code generator from LIR.
class AutoTranslator {
 public:
  static AutoTranslator& getInstance() {
    static AutoTranslator autotrans;
    return autotrans;
  }

  AutoTranslator(const AutoTranslator&) = delete;
  AutoTranslator& operator=(const AutoTranslator&) = delete;

  void translateInstr(Environ* env, const jit::lir::Instruction* instr) const;

  static arch::Gp getGp(lir::DataType data_type, PhyLocation reg) {
    JIT_CHECK(reg.isGpRegister(), "Expected a general-purpose register");
#if defined(CINDER_X86_64)
    auto reg_id = static_cast<uint32_t>(reg.loc);
    switch (data_type) {
      case jit::lir::Operand::k8bit:
        return asmjit::x86::gpb(reg_id);
      case jit::lir::Operand::k16bit:
        return asmjit::x86::gpw(reg_id);
      case jit::lir::Operand::k32bit:
        return asmjit::x86::gpd(reg_id);
      case jit::lir::Operand::kObject:
      case jit::lir::Operand::kObjectUntagged:
      case jit::lir::Operand::k64bit:
        return asmjit::x86::gpq(reg_id);
      case jit::lir::Operand::kDouble:
        JIT_ABORT("incorrect register type.");
    }
#elif defined(CINDER_AARCH64)
    auto reg_id =
        reg == XZR ? asmjit::a64::Gp::kIdZr : static_cast<uint32_t>(reg.loc);

    if (reg == raw(RegId::XZR)) {
      reg = asmjit::a64::Gp::kIdZr;
    }

    switch (data_type) {
      case jit::lir::Operand::k8bit:
      case jit::lir::Operand::k16bit:
        JIT_ABORT("Unsupported register size in aarch64.");
      case jit::lir::Operand::k32bit:
        return asmjit::a64::w(reg_id);
      case jit::lir::Operand::kObject:
      case jit::lir::Operand::kObjectUntagged:
      case jit::lir::Operand::k64bit:
        return asmjit::a64::x(reg_id);
      case jit::lir::Operand::kDouble:
        JIT_ABORT("incorrect register type.");
    }
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGp(const lir::Operand* op, PhyLocation reg) {
#if defined(CINDER_X86_64)
    return getGp(op->dataType(), reg);
#elif defined(CINDER_AARCH64)
    return getGp(op->dataType(), reg);
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGpOutput(const lir::Operand* op, PhyLocation reg) {
#if defined(CINDER_X86_64)
    return getGp(op->dataType(), reg);
#elif defined(CINDER_AARCH64)
    auto data_type = op->dataType();

    if (data_type == jit::lir::Operand::k8bit ||
        data_type == jit::lir::Operand::k16bit) {
      return getGp(jit::lir::Operand::k32bit, reg);
    }
    return getGp(op->dataType(), reg);
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::VecD getVecD(const jit::lir::Operand* op) {
#if defined(CINDER_X86_64)
    auto data_type = op->dataType();
    switch (data_type) {
      case jit::lir::Operand::kDouble:
        return asmjit::x86::xmm(op->getPhyRegister().loc - VECD_REG_BASE);
      default:
        JIT_ABORT("incorrect register type.");
    }
#elif defined(CINDER_AARCH64)
    auto data_type = op->dataType();
    switch (data_type) {
      case jit::lir::Operand::kDouble:
        return asmjit::a64::d(op->getPhyRegister().loc - VECD_REG_BASE);
      default:
        JIT_ABORT("incorrect register type.");
    }
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGp(const jit::lir::Operand* op) {
    return getGp(op, op->getPhyRegister());
  }

  static arch::Gp getGpOutput(const jit::lir::Operand* op) {
    return getGpOutput(op, op->getPhyRegister());
  }

  static arch::Gp getGpWiden(lir::DataType data_type, PhyLocation reg) {
    // AArch64 has no sub-32-bit GP registers. Values in registers are
    // guaranteed to be properly zero-extended by ldrb/ldrh/cset.
    // For signed operations, use the postgen sign-extension pass instead.
    if constexpr (kBuildArch == Arch::kAarch64) {
      if (data_type == jit::lir::Operand::k8bit ||
          data_type == jit::lir::Operand::k16bit) {
        data_type = jit::lir::Operand::k32bit;
      }
    }
    return getGp(data_type, reg);
  }

  static arch::Gp getGpWiden(const lir::Operand* op) {
    return getGpWiden(op->dataType(), op->getPhyRegister());
  }

 private:
  AutoTranslator() = default;
};

} // namespace cinderx::jit::codegen::autogen
