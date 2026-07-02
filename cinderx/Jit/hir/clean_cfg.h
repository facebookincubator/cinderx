// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Combination of passes to generally clean up the entire CFG.
class CleanCFG final : public Pass {
 public:
  CleanCFG() : Pass("CleanCFG") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<CleanCFG> factory() {
    return std::make_unique<CleanCFG>();
  }
};

} // namespace cinderx::jit::hir
