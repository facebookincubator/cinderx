// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Inserts incref/decref instructions.
class RefcountInsertion final : public Pass {
 public:
  RefcountInsertion() : Pass("RefcountInsertion") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<RefcountInsertion> factory() {
    return std::make_unique<RefcountInsertion>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(RefcountInsertion);
};

} // namespace cinderx::jit::hir
