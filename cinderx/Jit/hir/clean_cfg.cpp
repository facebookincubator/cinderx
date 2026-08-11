// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/clean_cfg.h"

#include "cinderx/Common/log.h"

namespace cinderx::jit::hir {

void CleanCFG::run(Function& irfunc) {
  constexpr size_t kRunLimit = 10;
  size_t run = 0;
  bool changed = false;

  for (; run < kRunLimit; ++run) {
    removeUnreachableInstructions(irfunc);

    bool modified = mergeLinearBlocks(irfunc);
    modified |= removeUnreachableBlocks(irfunc);
    changed |= modified;

    if (!modified) {
      break;
    }
  }

  JIT_THROW_IF(
      run == kRunLimit,
      "CleanCFG for function '{}' did not complete in the maximum number of "
      "runs ({})",
      irfunc.fullname,
      kRunLimit);

  if (changed) {
    reflowTypes(irfunc);
  }
}

} // namespace cinderx::jit::hir
