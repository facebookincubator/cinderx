// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Inserts incref/decref instructions.
class RefcountInsertion : public Pass {
 public:
  RefcountInsertion() : Pass("RefcountInsertion") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<RefcountInsertion> Factory() {
    return std::make_unique<RefcountInsertion>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(RefcountInsertion);
};

} // namespace jit::hir
