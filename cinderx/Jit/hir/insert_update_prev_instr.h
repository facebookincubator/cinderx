// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

class InsertUpdatePrevInstr final : public Pass {
 public:
  InsertUpdatePrevInstr() : Pass("InsertUpdatePrevInstr") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<InsertUpdatePrevInstr> factory() {
    return std::make_unique<InsertUpdatePrevInstr>();
  }
};

} // namespace cinderx::jit::hir
