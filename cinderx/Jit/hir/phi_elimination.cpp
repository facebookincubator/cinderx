// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/phi_elimination.h"

#include "cinderx/Jit/hir/copy_propagation.h"

namespace jit::hir {

void PhiElimination::Run(Function& func) {
  for (bool changed = true; changed;) {
    changed = false;

    for (auto& block : func.cfg.blocks) {
      std::vector<Instr*> assigns_or_loads;
      for (auto it = block.begin(); it != block.end();) {
        auto& instr = *it;
        ++it;
        if (!instr.IsPhi()) {
          for (auto assign : assigns_or_loads) {
            assign->InsertBefore(instr);
          }
          break;
        }
        if (auto value = static_cast<Phi&>(instr).isTrivial()) {
          // If a trivial Phi references itself then it can never be
          // initialized, and we can use a LoadConst<Bottom> to signify that.
          Register* model_value = chaseAssignOperand(value);
          Instr* new_instr;
          if (model_value == instr.output()) {
            new_instr = LoadConst::create(instr.output(), TBottom);
          } else {
            new_instr = Assign::create(instr.output(), value);
          }
          new_instr->copyBytecodeOffset(instr);
          assigns_or_loads.emplace_back(new_instr);
          instr.unlink();
          delete &instr;
          changed = true;
        }
      }
    }

    CopyPropagation{}.Run(func);
  }

  // Consider having a separate run of CleanCFG between passes clean this up.
  removeTrampolineBlocks(&func.cfg);
}

} // namespace jit::hir
