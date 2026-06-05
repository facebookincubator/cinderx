// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Populates live-value metadata for HIR instructions that lower to helper calls
// requiring deferred-RC GC root recovery.
class CallSiteLiveValues : public Pass {
 public:
  CallSiteLiveValues() : Pass("CallSiteLiveValues") {}

  void Run(Function& irfunc) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(CallSiteLiveValues);
};

} // namespace jit::hir
