// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

class GuardTypeRemoval final : public Pass {
 public:
  GuardTypeRemoval() : Pass("GuardTypeRemoval") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<GuardTypeRemoval> factory() {
    return std::make_unique<GuardTypeRemoval>();
  }
};

} // namespace cinderx::jit::hir
