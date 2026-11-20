// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Reject functions which call the builtin exec() or eval() without explicit
// globals and locals.  Those calls read the calling frame's namespaces, which
// JIT frames don't keep up to date.
//
// Must run before Simplify, which can rewrite calls into forms that no longer
// expose the callee and argument count.  Throws if a banned call is found.
class CheckBannedCalls final : public Pass {
 public:
  CheckBannedCalls() : Pass("CheckBannedCalls") {}

  bool run(Function& func) override;

  static std::unique_ptr<CheckBannedCalls> factory() {
    return std::make_unique<CheckBannedCalls>();
  }
};

} // namespace cinderx::jit::hir
