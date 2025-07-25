// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

Register* chaseAssignOperand(Register* value) {
  while (value->instr()->IsAssign()) {
    value = value->instr()->GetOperand(0);
  }
  return value;
}

} // namespace jit::hir
