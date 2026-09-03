// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Inserts incref/decref instructions.
class RefcountInsertion final : public Pass {
 public:
  RefcountInsertion() : Pass("RefcountInsertion") {}

  RefcountInsertion(const RefcountInsertion&) = delete;
  RefcountInsertion& operator=(const RefcountInsertion&) = delete;

  void run(Function& irfunc) override;

  static std::unique_ptr<RefcountInsertion> factory() {
    return std::make_unique<RefcountInsertion>();
  }
};

} // namespace cinderx::jit::hir
