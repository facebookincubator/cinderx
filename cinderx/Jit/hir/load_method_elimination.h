// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

class LoadMethodElimination final : public Pass {
 public:
  LoadMethodElimination() : Pass("LoadMethodElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<LoadMethodElimination> factory() {
    return std::make_unique<LoadMethodElimination>();
  }
};

} // namespace cinderx::jit::hir
