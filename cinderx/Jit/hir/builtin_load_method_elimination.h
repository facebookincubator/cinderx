// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

class BuiltinLoadMethodElimination final : public Pass {
 public:
  BuiltinLoadMethodElimination() : Pass("BuiltinLoadMethodElimination") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<BuiltinLoadMethodElimination> factory() {
    return std::make_unique<BuiltinLoadMethodElimination>();
  }
};

} // namespace cinderx::jit::hir
