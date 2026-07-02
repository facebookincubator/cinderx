// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"

#include "cinderx/Jit/hir/analysis.h"

namespace cinderx::jit::hir {

namespace {

Instr* replaceCompare(Compare* compare, IsTruthy* truthy) {
  return CompareBool::create(
      truthy->output(),
      compare->op(),
      compare->getOperand(0),
      compare->getOperand(1),
      *get_frame_state(*truthy));
}

} // namespace

void DynamicComparisonElimination::Run(Function& irfunc) {
  LivenessAnalysis liveness{irfunc};
  liveness.run();
  auto last_uses = liveness.getLastUses();

  // Optimize "if x is y" case
  for (auto& block : irfunc.cfg.blocks) {
    auto& instr = block.back();

    // Looking for:
    //   $some_conditional = ...
    //   $truthy = IsTruthy $compare
    //   CondBranch<x, y> $truthy
    // Which we then re-write to a form which doesn't use IsTruthy anymore.
    if (!instr.isCondBranch()) {
      continue;
    }

    Instr* truthy = instr.getOperand(0)->instr();
    if (!truthy->isIsTruthy() || truthy->block() != &block) {
      continue;
    }

    Instr* truthy_target = truthy->getOperand(0)->instr();
    if (truthy_target->block() != &block ||
        (!truthy_target->isCompare() && !truthy_target->isVectorCall())) {
      continue;
    }

    auto& dying_regs = map_get(last_uses, truthy, kEmptyRegSet);

    if (!dying_regs.contains(truthy->getOperand(0))) {
      // Compare output lives on, we can't re-write...
      continue;
    }

    // Make sure the output of compare isn't getting used between the compare
    // and the branch other than by the truthy instruction.
    std::vector<Instr*> snapshots;
    bool can_optimize = true;
    for (auto it = std::next(block.rbegin()); it != block.rend(); ++it) {
      if (&*it == truthy_target) {
        break;
      } else if (&*it != truthy) {
        if (it->isSnapshot()) {
          if (it->uses(truthy_target->output())) {
            snapshots.push_back(&*it);
          }
          continue;
        } else if (!it->isReplayable()) {
          can_optimize = false;
          break;
        }

        if (it->uses(truthy->getOperand(0))) {
          can_optimize = false;
          break;
        }
      }
    }
    if (!can_optimize) {
      continue;
    }

    Instr* replacement = nullptr;
    if (truthy_target->isCompare()) {
      auto compare = static_cast<Compare*>(truthy_target);

      replacement = replaceCompare(compare, static_cast<IsTruthy*>(truthy));
    }

    if (replacement != nullptr) {
      replacement->copyBytecodeOffset(instr);
      truthy->replaceWith(*replacement);

      truthy_target->unlink();
      delete truthy_target;
      delete truthy;

      // There may be zero or more Snapshots between the Compare and the
      // IsTruthy that uses the output of the Compare (which we want to delete).
      // Since we're fusing the two operations together, the Snapshot and
      // its use of the dead intermediate value should be deleted.
      for (auto snapshot : snapshots) {
        snapshot->unlink();
        delete snapshot;
      }
    }
  }

  reflowTypes(irfunc);
}

} // namespace cinderx::jit::hir
