// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Remove Phis that only have one unique input value (other than their output).
class PhiElimination : public Pass {
 public:
  PhiElimination() : Pass("PhiElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<PhiElimination> Factory() {
    return std::make_unique<PhiElimination>();
  }
};

} // namespace jit::hir
