// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Inline function calls and add in BeginInlinedFunction and EndInlinedFunction
// instructions.
class InlineFunctionCalls : public Pass {
 public:
  InlineFunctionCalls() : Pass("InlineFunctionCalls") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<InlineFunctionCalls> Factory() {
    return std::make_unique<InlineFunctionCalls>();
  }
};

// Try to elide {Begin,End}InlinedFunction instructions for simple functions
// that will never need a Python frame.
class BeginInlinedFunctionElimination : public Pass {
 public:
  BeginInlinedFunctionElimination() : Pass("BeginInlinedFunctionElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<BeginInlinedFunctionElimination> Factory() {
    return std::make_unique<BeginInlinedFunctionElimination>();
  }
};

} // namespace jit::hir
