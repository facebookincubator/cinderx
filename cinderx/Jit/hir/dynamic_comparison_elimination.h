// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

class DynamicComparisonElimination : public Pass {
 public:
  DynamicComparisonElimination() : Pass("DynamicComparisonElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<DynamicComparisonElimination> Factory() {
    return std::make_unique<DynamicComparisonElimination>();
  }
};

} // namespace jit::hir
