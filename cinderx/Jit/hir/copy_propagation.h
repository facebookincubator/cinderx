// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Eliminate Assign instructions by propagating copies.
class CopyPropagation final : public Pass {
 public:
  CopyPropagation() : Pass("CopyPropagation") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<CopyPropagation> factory() {
    return std::make_unique<CopyPropagation>();
  }
};

} // namespace cinderx::jit::hir
