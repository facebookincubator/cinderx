// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"

namespace jit::codegen::autogen {

// A machine code generator from LIR.
class AutoTranslator {
 public:
  static AutoTranslator& getInstance() {
    static AutoTranslator autotrans;
    return autotrans;
  }

  void translateInstr(Environ* env, const jit::lir::Instruction* instr) const;

  static arch::Gp getGp(lir::DataType data_type, unsigned int reg) {
#if defined(CINDER_X86_64)
    switch (data_type) {
      case jit::lir::OperandBase::k8bit:
        return asmjit::x86::gpb(reg);
      case jit::lir::OperandBase::k16bit:
        return asmjit::x86::gpw(reg);
      case jit::lir::OperandBase::k32bit:
        return asmjit::x86::gpd(reg);
      case jit::lir::OperandBase::kObject:
      case jit::lir::OperandBase::k64bit:
        return asmjit::x86::gpq(reg);
      case jit::lir::OperandBase::kDouble:
        JIT_ABORT("incorrect register type.");
    }
#elif defined(CINDER_AARCH64)
    JIT_CHECK(reg != raw(RegId::SP), "SP is not a general-purpose register");

    switch (data_type) {
      case jit::lir::OperandBase::k8bit:
      case jit::lir::OperandBase::k16bit:
        JIT_ABORT("Unsupported register size in aarch64.");
      case jit::lir::OperandBase::k32bit:
        return asmjit::a64::w(reg);
      case jit::lir::OperandBase::kObject:
      case jit::lir::OperandBase::k64bit:
        return asmjit::a64::x(reg);
      case jit::lir::OperandBase::kDouble:
        JIT_ABORT("incorrect register type.");
    }
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGp(const lir::OperandBase* op, unsigned int reg) {
#if defined(CINDER_X86_64)
    return getGp(op->dataType(), reg);
#elif defined(CINDER_AARCH64)
    JIT_CHECK(reg != raw(RegId::SP), "SP is not a general-purpose register");
    return getGp(op->dataType(), reg);
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGpOutput(const lir::OperandBase* op, unsigned int reg) {
#if defined(CINDER_X86_64)
    return getGp(op->dataType(), reg);
#elif defined(CINDER_AARCH64)
    JIT_CHECK(reg != raw(RegId::SP), "SP is not a general-purpose register");
    auto data_type = op->dataType();

    if (data_type == jit::lir::OperandBase::k8bit ||
        data_type == jit::lir::OperandBase::k16bit) {
      return asmjit::a64::w(reg);
    }
    return getGp(op->dataType(), reg);
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::VecD getVecD(const jit::lir::OperandBase* op) {
#if defined(CINDER_X86_64)
    auto data_type = op->dataType();
    switch (data_type) {
      case jit::lir::OperandBase::kDouble:
        return asmjit::x86::xmm(op->getPhyRegister().loc - VECD_REG_BASE);
      default:
        JIT_ABORT("incorrect register type.");
    }
#elif defined(CINDER_AARCH64)
    auto data_type = op->dataType();
    switch (data_type) {
      case jit::lir::OperandBase::kDouble:
        return asmjit::a64::d(op->getPhyRegister().loc - VECD_REG_BASE);
      default:
        JIT_ABORT("incorrect register type.");
    }
#else
    CINDER_UNSUPPORTED
#endif
    Py_UNREACHABLE();
  }

  static arch::Gp getGp(const jit::lir::OperandBase* op) {
    return getGp(op, op->getPhyRegister().loc);
  }

  static arch::Gp getGpOutput(const jit::lir::OperandBase* op) {
    return getGpOutput(op, op->getPhyRegister().loc);
  }

  static arch::Gp getGpWiden(lir::DataType data_type, unsigned int reg) {
    // AArch64 has no sub-32-bit GP registers. Values in registers are
    // guaranteed to be properly zero-extended by ldrb/ldrh/cset.
    // For signed operations, use the postgen sign-extension pass instead.
    if constexpr (arch::kBuildArch == arch::Arch::kAarch64) {
      if (data_type == jit::lir::OperandBase::k8bit ||
          data_type == jit::lir::OperandBase::k16bit) {
        data_type = jit::lir::OperandBase::k32bit;
      }
    }
    return getGp(data_type, reg);
  }

  static arch::Gp getGpWiden(const lir::OperandBase* op) {
    return getGpWiden(op->dataType(), op->getPhyRegister().loc);
  }

 private:
  AutoTranslator() = default;

  DISALLOW_COPY_AND_ASSIGN(AutoTranslator);
};

} // namespace jit::codegen::autogen
