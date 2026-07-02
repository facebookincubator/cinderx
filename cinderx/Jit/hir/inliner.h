// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Inline function calls and add in BeginInlinedFunction and EndInlinedFunction
// instructions.
class InlineFunctionCalls final : public Pass {
 public:
  InlineFunctionCalls() : Pass("InlineFunctionCalls") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<InlineFunctionCalls> factory() {
    return std::make_unique<InlineFunctionCalls>();
  }
};

// Try to elide {Begin,End}InlinedFunction instructions for simple functions
// that will never need a Python frame.
class BeginInlinedFunctionElimination final : public Pass {
 public:
  BeginInlinedFunctionElimination() : Pass("BeginInlinedFunctionElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<BeginInlinedFunctionElimination> factory() {
    return std::make_unique<BeginInlinedFunctionElimination>();
  }
};

} // namespace cinderx::jit::hir
