// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/runtime.h"

#include <asmjit/asmjit.h>

#include <vector>

namespace jit::codegen {

class FrameAsm {
 public:
  FrameAsm(const hir::Function* func, Environ& env) : func_(func), env_(env) {}

  void initializeFrameHeader(arch::Gp tstate_reg, arch::Gp scratch_reg);

  // Generates the code to link the Python stack frame in. This will ensure
  // that the save_regs get transformed from the first of the pair to the
  // second of the pair. It will also initialize thread state and leave
  // it in tstate.
  void generateLinkFrame(
      const arch::Gp& func_reg,
      const arch::Gp& tstate_reg,
      const std::vector<std::pair<const arch::Reg&, const arch::Reg&>>&
          save_regs);

  void generateUnlinkFrame(bool is_generator);

  void setAssembler(arch::Builder* as) {
    as_ = as;
  }

  int frameHeaderSize();

 private:
  const hir::Function* GetFunction() const {
    return func_;
  }

  CodeRuntime* codeRuntime() const {
    return env_.code_rt;
  }

  bool isGen() const {
    return func_->code->co_flags & kCoFlagsAnyGenerator;
  }

  void emitIncTotalRefCount(const arch::Gp& scratch_reg);
  void incRef(const arch::Gp& reg, const arch::Gp& scratch_reg);
  bool storeConst(
      const arch::Gp& reg,
      int32_t offset,
      void* val,
      const arch::Gp& scratch);

  void loadTState(const arch::Gp& dst_reg);
  void linkNormalGeneratorFrame(
      RegisterPreserver& preserver,
      const arch::Gp& func_reg,
      const arch::Gp& tstate_reg);
  void linkLightWeightFunctionFrame(
      RegisterPreserver& preserver,
      const arch::Gp& func_reg,
      const arch::Gp& tstate_reg);
  void linkNormalFunctionFrame(
      RegisterPreserver& preserver,
      const arch::Gp& func_reg,
      const arch::Gp& tstate_reg);
  void linkNormalFrame(
      RegisterPreserver& preserver,
      const arch::Gp& func_reg,
      const arch::Gp& tstate_reg);
  void linkOnStackShadowFrame(
      const arch::Gp& tstate_reg,
      const arch::Gp& scratch_reg);

  arch::Builder* as_{};
  const hir::Function* func_;
  Environ& env_;

  int frameHeaderSizeExcludingSpillSpace() const;
};
} // namespace jit::codegen
