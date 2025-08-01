// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Eliminate Assign instructions by propagating copies.
class CopyPropagation : public Pass {
 public:
  CopyPropagation() : Pass("CopyPropagation") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<CopyPropagation> Factory() {
    return std::make_unique<CopyPropagation>();
  }
};

} // namespace jit::hir
