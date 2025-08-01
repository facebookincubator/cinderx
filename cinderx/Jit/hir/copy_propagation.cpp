// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/copy_propagation.h"

namespace jit::hir {

void CopyPropagation::Run(Function& irfunc) {
  std::vector<Instr*> assigns;
  for (auto block : irfunc.cfg.GetRPOTraversal()) {
    for (auto& instr : *block) {
      instr.visitUses([](Register*& reg) {
        reg = chaseAssignOperand(reg);
        return true;
      });

      if (instr.IsAssign()) {
        assigns.emplace_back(&instr);
      }
    }
  }

  for (auto instr : assigns) {
    instr->unlink();
    delete instr;
  }
}

} // namespace jit::hir
