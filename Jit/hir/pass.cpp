// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

Register* chaseAssignOperand(Register* value) {
  while (value->instr()->IsAssign()) {
    value = value->instr()->GetOperand(0);
  }
  return value;
}

void simplifyRedundantCondBranches(CFG* cfg) {
  std::vector<BasicBlock*> to_simplify;
  for (auto& block : cfg->blocks) {
    if (block.empty()) {
      continue;
    }
    auto term = block.GetTerminator();
    std::size_t num_edges = term->numEdges();
    if (num_edges < 2) {
      continue;
    }
    JIT_CHECK(num_edges == 2, "only two edges are supported");
    if (term->successor(0) != term->successor(1)) {
      continue;
    }
    switch (term->opcode()) {
      case Opcode::kCondBranch:
      case Opcode::kCondBranchIterNotDone:
      case Opcode::kCondBranchCheckType:
        break;
      default:
        // Can't be sure that it's safe to replace the instruction with a branch
        JIT_ABORT("Unknown side effects of {} instruction", term->opname());
    }
    to_simplify.emplace_back(&block);
  }
  for (auto& block : to_simplify) {
    auto term = block->GetTerminator();
    term->unlink();
    auto branch = block->appendWithOff<Branch>(
        term->bytecodeOffset(), term->successor(0));
    branch->copyBytecodeOffset(*term);
    delete term;
  }
}

bool removeTrampolineBlocks(CFG* cfg) {
  std::vector<BasicBlock*> trampolines;
  for (auto& block : cfg->blocks) {
    if (!block.IsTrampoline()) {
      continue;
    }
    BasicBlock* succ = block.successor(0);
    // if this is the entry block and its successor has multiple
    // predecessors, don't remove it; it's necessary to maintain isolated
    // entries
    if (&block == cfg->entry_block) {
      if (succ->in_edges().size() > 1) {
        continue;
      } else {
        cfg->entry_block = succ;
      }
    }
    // Update all predecessors to jump directly to our successor
    block.retargetPreds(succ);
    // Finish splicing the trampoline out of the cfg
    block.set_successor(0, nullptr);
    trampolines.emplace_back(&block);
  }
  for (auto& block : trampolines) {
    cfg->RemoveBlock(block);
    delete block;
  }
  simplifyRedundantCondBranches(cfg);
  return trampolines.size() > 0;
}

bool removeUnreachableBlocks(CFG* cfg) {
  std::unordered_set<BasicBlock*> visited;
  std::vector<BasicBlock*> stack;
  stack.emplace_back(cfg->entry_block);
  while (!stack.empty()) {
    BasicBlock* block = stack.back();
    stack.pop_back();
    if (visited.contains(block)) {
      continue;
    }
    visited.insert(block);
    auto term = block->GetTerminator();
    for (std::size_t i = 0, n = term->numEdges(); i < n; ++i) {
      BasicBlock* succ = term->successor(i);
      // This check isn't necessary for correctness but avoids unnecessary
      // pushes to the stack.
      if (!visited.contains(succ)) {
        stack.emplace_back(succ);
      }
    }
  }

  std::vector<BasicBlock*> unreachable;
  for (auto it = cfg->blocks.begin(); it != cfg->blocks.end();) {
    BasicBlock* block = &*it;
    ++it;
    if (!visited.contains(block)) {
      if (Instr* old_term = block->GetTerminator()) {
        for (std::size_t i = 0, n = old_term->numEdges(); i < n; ++i) {
          old_term->successor(i)->removePhiPredecessor(block);
        }
      }
      cfg->RemoveBlock(block);
      block->clear();
      unreachable.emplace_back(block);
    }
  }

  for (BasicBlock* block : unreachable) {
    delete block;
  }

  return unreachable.size() > 0;
}

} // namespace jit::hir
