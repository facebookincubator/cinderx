// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"

#include "cinderx/Jit/hir/analysis.h"

#include <optional>
#include <vector>

namespace cinderx::jit::hir {

namespace {

// Check that `compare` can be fused into `truthy`, which must both live in
// `block`.  Every instruction after `compare` up to the block's terminator,
// other than `truthy`, must be replayable and must not use the compare's
// output.  Snapshots are exempt and are returned instead, as their uses of the
// output must go away along with `compare`.
//
// Returns std::nullopt if the fusion isn't safe.
std::optional<std::vector<Instr*>> findSnapshotsToRemove(
    BasicBlock& block,
    const Compare& compare,
    const IsTruthy& truthy) {
  Register* compare_out = compare.output();
  std::vector<Instr*> snapshots;
  for (auto it = std::next(block.rbegin()); it != block.rend(); ++it) {
    Instr& instr = *it;
    if (&instr == &compare) {
      return snapshots;
    }
    if (&instr == &truthy) {
      continue;
    }
    if (instr.isSnapshot()) {
      if (instr.uses(compare_out)) {
        snapshots.push_back(&instr);
      }
      continue;
    }
    if (!instr.isReplayable() || instr.uses(compare_out)) {
      return std::nullopt;
    }
  }
  return snapshots;
}

} // namespace

bool DynamicComparisonElimination::run(Function& irfunc) {
  LivenessAnalysis liveness{irfunc};
  liveness.run();
  auto last_uses = liveness.getLastUses();

  bool changed = false;
  for (auto& block : irfunc.cfg.blocks) {
    auto& branch = block.back();

    // Looking for:
    //   $compare = Compare<op> $left $right
    //   $truthy = IsTruthy $compare
    //   CondBranch<x, y> $truthy
    // Which we then rewrite to:
    //   $truthy = CompareBool<op> $left $right
    //   CondBranch<x, y> $truthy
    if (!branch.isCondBranch()) {
      continue;
    }

    Instr* truthy_instr = branch.getOperand(0)->instr();
    if (!truthy_instr->isIsTruthy() || truthy_instr->block() != &block) {
      continue;
    }
    auto truthy = static_cast<IsTruthy*>(truthy_instr);

    Instr* compare_instr = truthy->getOperand(0)->instr();
    if (!compare_instr->isCompare() || compare_instr->block() != &block) {
      continue;
    }
    auto compare = static_cast<Compare*>(compare_instr);

    // The compare's output can't be used past the IsTruthy.
    auto& dying_regs = map_get(last_uses, truthy, kEmptyRegSet);
    if (!dying_regs.contains(compare->output())) {
      continue;
    }

    std::optional<std::vector<Instr*>> snapshots =
        findSnapshotsToRemove(block, *compare, *truthy);
    if (!snapshots.has_value()) {
      continue;
    }

    Instr* replacement = CompareBool::create(
        truthy->output(),
        compare->op(),
        compare->getOperand(0),
        compare->getOperand(1),
        *get_frame_state(*truthy));
    replacement->copyBytecodeOffset(branch);
    truthy->replaceWith(*replacement);
    delete truthy;

    compare->unlink();
    delete compare;

    for (Instr* snapshot : *snapshots) {
      snapshot->unlink();
      delete snapshot;
    }

    changed = true;
  }

  if (changed) {
    reflowTypes(irfunc);
  }
  return changed;
}

} // namespace cinderx::jit::hir
