// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/clean_cfg.h"

#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"

namespace cinderx::jit::hir {

namespace {

bool absorbDstBlock(BasicBlock* block) {
  if (block->getTerminator()->opcode() != Opcode::kBranch) {
    return false;
  }
  auto branch = dynamic_cast<Branch*>(block->getTerminator());
  BasicBlock* target = branch->target();
  if (target == block) {
    return false;
  }
  if (target->inEdges().size() != 1) {
    return false;
  }
  if (target == block) {
    return false;
  }
  branch->unlink();
  while (!target->empty()) {
    Instr* instr = target->pop_front();
    JIT_CHECK(!instr->isPhi(), "Expected no Phi but found {}", *instr);
    block->append(instr);
  }
  // The successors to target might have Phis that still refer to target.
  // Retarget them to refer to block.
  Instr* old_term = block->getTerminator();
  JIT_CHECK(old_term != nullptr, "block must have a terminator");
  for (std::size_t i = 0, n = old_term->numEdges(); i < n; ++i) {
    old_term->successor(i)->fixupPhis(
        /*old_pred=*/target, /*new_pred=*/block);
  }
  // Target block becomes unreachable and gets picked up by
  // removeUnreachableBlocks.
  delete branch;
  return true;
}

} // namespace

void CleanCFG::run(Function& irfunc) {
  bool changed = false;

  do {
    removeUnreachableInstructions(irfunc);
    // Remove any trivial Phis; absorbDstBlock cannot handle them.
    PhiElimination{}.run(irfunc);
    std::vector<BasicBlock*> blocks = irfunc.cfg.getRPOTraversal();
    for (auto block : blocks) {
      // Ignore transient empty blocks.
      if (block->empty()) {
        continue;
      }
      // Keep working on the current block until no further changes are made.
      for (;; changed = true) {
        if (absorbDstBlock(block)) {
          continue;
        }
        break;
      }
    }
  } while (removeUnreachableBlocks(irfunc));

  if (changed) {
    reflowTypes(irfunc);
  }
}

} // namespace cinderx::jit::hir
