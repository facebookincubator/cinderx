// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Populates live-value metadata for HIR instructions that lower to helper calls
// requiring deferred-RC GC root recovery.
class CallSiteLiveValues final : public Pass {
 public:
  CallSiteLiveValues() : Pass("CallSiteLiveValues") {}

  void run(Function& irfunc) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(CallSiteLiveValues);
};

} // namespace cinderx::jit::hir
