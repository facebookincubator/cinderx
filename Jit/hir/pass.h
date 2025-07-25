// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"

#include <string>
#include <string_view>

namespace jit::hir {

class Pass {
 public:
  explicit Pass(std::string_view name) : name_{name} {}
  virtual ~Pass() = default;

  virtual void Run(Function& irfunc) = 0;

  constexpr std::string_view name() const {
    return name_;
  }

 protected:
  std::string name_;
};

// Recursively chase a list of assignments and get the original register value.
// If there are no assignments then just get the register back.
Register* chaseAssignOperand(Register* value);

// Replace cond branches where both sides go to the same block with a direct
// branch.
void simplifyRedundantCondBranches(CFG* cfg);

// Remove any blocks that consist of a single jump to another block. Avoid using
// this alone; use CleanCFG instead.
bool removeTrampolineBlocks(CFG* cfg);

} // namespace jit::hir
