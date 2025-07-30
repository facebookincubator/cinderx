// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Combination of passes to generally clean up the entire CFG.
class CleanCFG : public Pass {
 public:
  CleanCFG() : Pass("CleanCFG") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<CleanCFG> Factory() {
    return std::make_unique<CleanCFG>();
  }
};

} // namespace jit::hir
