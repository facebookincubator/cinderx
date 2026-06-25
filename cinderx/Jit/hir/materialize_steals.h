// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

class MaterializeSteals final : public Pass {
 public:
  MaterializeSteals() : Pass("MaterializeSteals") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<MaterializeSteals> Factory() {
    return std::make_unique<MaterializeSteals>();
  }
};

} // namespace cinderx::jit::hir
