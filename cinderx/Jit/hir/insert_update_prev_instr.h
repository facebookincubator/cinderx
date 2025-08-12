// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

class InsertUpdatePrevInstr : public Pass {
 public:
  InsertUpdatePrevInstr() : Pass("InsertUpdatePrevInstr") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<InsertUpdatePrevInstr> Factory() {
    return std::make_unique<InsertUpdatePrevInstr>();
  }
};

} // namespace jit::hir
