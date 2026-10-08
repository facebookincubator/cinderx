// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/phi_elimination.h"

#include "cinderx/Jit/hir/copy_propagation.h"

namespace cinderx::jit::hir {

bool PhiElimination::run(Function& func) {
  size_t deleted = 0;
  size_t prev_deleted;

  do {
    prev_deleted = deleted;

    for (auto& block : func.cfg.blocks) {
      std::vector<Instr*> assigns_or_loads;
      for (auto it = block.begin(); it != block.end();) {
        auto& instr = *it;
        ++it;
        if (!instr.isPhi()) {
          for (auto assign : assigns_or_loads) {
            assign->insertBefore(instr);
          }
          break;
        }
        if (auto new_instr = collapseTrivialPhi(static_cast<Phi&>(instr))) {
          new_instr->copyBytecodeOffset(instr);
          assigns_or_loads.emplace_back(new_instr);
          instr.unlink();
          delete &instr;
          deleted += 1;
        }
      }
    }

    CopyPropagation{}.run(func);
  } while (deleted != prev_deleted);

  // Consider having a separate run of CleanCFG between passes clean this up.
  mergeLinearBlocks(func);

  return deleted > 0;
}

} // namespace cinderx::jit::hir
