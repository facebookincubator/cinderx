// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Common subexpression elimination for pure value computations: when an
// instruction computes the same value as a dominating instruction (same
// opcode, same inputs, same immediates), its uses are rewritten to the
// dominating instruction's output and it is removed.
class CommonSubexpressionElimination final : public Pass {
 public:
  CommonSubexpressionElimination() : Pass("CommonSubexpressionElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<CommonSubexpressionElimination> factory() {
    return std::make_unique<CommonSubexpressionElimination>();
  }
};

} // namespace cinderx::jit::hir
