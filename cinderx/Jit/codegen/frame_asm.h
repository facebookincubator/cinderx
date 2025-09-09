// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/runtime.h"

#include <asmjit/asmjit.h>

#include <vector>

namespace jit::codegen {

class FrameAsm {
 public:
  FrameAsm(const hir::Function* func, Environ& env) : func_(func), env_(env) {}

  void initializeFrameHeader(
      asmjit::x86::Gp tstate_reg,
      asmjit::x86::Gp scratch_reg);

  // Generates the code to link the Python stack frame in. This will ensure
  // that the save_regs get transformed from the first of the pair to the
  // second of the pair. It will also initialize thread state and leave
  // it in tstate.
  void generateLinkFrame(
      const asmjit::x86::Gp& func_reg,
      const asmjit::x86::Gp& tstate_reg,
      const std::vector<
          std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>&
          save_regs);

  void generateUnlinkFrame(bool is_generator);

  void setAssembler(asmjit::x86::Builder* as) {
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

  void emitIncTotalRefCount(const asmjit::x86::Gp& scratch_reg);
  void incRef(const asmjit::x86::Gp& reg, const asmjit::x86::Gp& scratch_reg);
  bool storeConst(
      const asmjit::x86::Gp& reg,
      int32_t offset,
      void* val,
      const asmjit::x86::Gp& scratch);

  void loadTState(const asmjit::x86::Gp& dst_reg, RegisterPreserver& preserver);
  void linkNormalGeneratorFrame(
      RegisterPreserver& preserver,
      const asmjit::x86::Gp& func_reg,
      const asmjit::x86::Gp& tstate_reg);
  void linkLightWeightFunctionFrame(
      RegisterPreserver& preserver,
      const asmjit::x86::Gp& func_reg,
      const asmjit::x86::Gp& tstate_reg);
  void linkNormalFunctionFrame(
      RegisterPreserver& preserver,
      const asmjit::x86::Gp& func_reg,
      const asmjit::x86::Gp& tstate_reg);
  void linkNormalFrame(
      RegisterPreserver& preserver,
      const asmjit::x86::Gp& func_reg,
      const asmjit::x86::Gp& tstate_reg);
  void linkOnStackShadowFrame(
      const asmjit::x86::Gp& tstate_reg,
      const asmjit::x86::Gp& scratch_reg);

  asmjit::x86::Builder* as_{};
  const hir::Function* func_;
  Environ& env_;

  int frameHeaderSizeExcludingSpillSpace() const;
};
} // namespace jit::codegen
