// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Remove Phis that only have one unique input value (other than their output).
class PhiElimination final : public Pass {
 public:
  PhiElimination() : Pass("PhiElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<PhiElimination> factory() {
    return std::make_unique<PhiElimination>();
  }
};

} // namespace cinderx::jit::hir
