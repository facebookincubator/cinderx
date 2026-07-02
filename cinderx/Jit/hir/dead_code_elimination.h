// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Eliminate instructions whose outputs are not used in a return or by
// other instructions with side-effects
class DeadCodeElimination final : public Pass {
 public:
  DeadCodeElimination() : Pass("DeadCodeElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<DeadCodeElimination> factory() {
    return std::make_unique<DeadCodeElimination>();
  }
};

} // namespace cinderx::jit::hir
