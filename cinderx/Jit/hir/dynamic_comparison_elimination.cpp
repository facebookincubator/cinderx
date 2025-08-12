// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"

#include "cinderx/Jit/hir/analysis.h"

namespace jit::hir {

namespace {

Instr* replaceCompare(Compare* compare, IsTruthy* truthy) {
  return CompareBool::create(
      truthy->output(),
      compare->op(),
      compare->GetOperand(0),
      compare->GetOperand(1),
      *get_frame_state(*truthy));
}

} // namespace

void DynamicComparisonElimination::Run(Function& irfunc) {
  LivenessAnalysis liveness{irfunc};
  liveness.Run();
  auto last_uses = liveness.GetLastUses();

  // Optimize "if x is y" case
  for (auto& block : irfunc.cfg.blocks) {
    auto& instr = block.back();

    // Looking for:
    //   $some_conditional = ...
    //   $truthy = IsTruthy $compare
    //   CondBranch<x, y> $truthy
    // Which we then re-write to a form which doesn't use IsTruthy anymore.
    if (!instr.IsCondBranch()) {
      continue;
    }

    Instr* truthy = instr.GetOperand(0)->instr();
    if (!truthy->IsIsTruthy() || truthy->block() != &block) {
      continue;
    }

    Instr* truthy_target = truthy->GetOperand(0)->instr();
    if (truthy_target->block() != &block ||
        (!truthy_target->IsCompare() && !truthy_target->IsVectorCall())) {
      continue;
    }

    auto& dying_regs = map_get(last_uses, truthy, kEmptyRegSet);

    if (!dying_regs.contains(truthy->GetOperand(0))) {
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
        if (it->IsSnapshot()) {
          if (it->Uses(truthy_target->output())) {
            snapshots.push_back(&*it);
          }
          continue;
        } else if (!it->isReplayable()) {
          can_optimize = false;
          break;
        }

        if (it->Uses(truthy->GetOperand(0))) {
          can_optimize = false;
          break;
        }
      }
    }
    if (!can_optimize) {
      continue;
    }

    Instr* replacement = nullptr;
    if (truthy_target->IsCompare()) {
      auto compare = static_cast<Compare*>(truthy_target);

      replacement = replaceCompare(compare, static_cast<IsTruthy*>(truthy));
    }

    if (replacement != nullptr) {
      replacement->copyBytecodeOffset(instr);
      truthy->ReplaceWith(*replacement);

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

} // namespace jit::hir
