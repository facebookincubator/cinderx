// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

class BuiltinLoadMethodElimination : public Pass {
 public:
  BuiltinLoadMethodElimination() : Pass("BuiltinLoadMethodElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<BuiltinLoadMethodElimination> Factory() {
    return std::make_unique<BuiltinLoadMethodElimination>();
  }
};

} // namespace jit::hir
