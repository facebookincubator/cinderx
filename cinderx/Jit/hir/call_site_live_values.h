// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Populates live-value metadata for HIR instructions that lower to helper calls
// requiring deferred-RC GC root recovery.
class CallSiteLiveValues final : public Pass {
 public:
  CallSiteLiveValues() : Pass("CallSiteLiveValues") {}

  CallSiteLiveValues(const CallSiteLiveValues&) = delete;
  CallSiteLiveValues& operator=(const CallSiteLiveValues&) = delete;

  void run(Function& irfunc) override;
};

} // namespace cinderx::jit::hir
