// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Perform a mixed bag of strength-reduction optimizations: remove redundant
// null checks, conversions, loads from compile-time constant containers, etc.
//
// If your optimization requires no global analysis or state and operates on
// one instruction at a time by inspecting its inputs (and anything reachable
// from them), it may be a good fit for Simplify.
class Simplify final : public Pass {
 public:
  Simplify() : Pass("Simplify") {}

  void run(Function& func) override;

  static std::unique_ptr<Simplify> factory() {
    return std::make_unique<Simplify>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(Simplify);
};

} // namespace cinderx::jit::hir
