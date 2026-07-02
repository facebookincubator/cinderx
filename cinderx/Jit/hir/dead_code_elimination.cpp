// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/dead_code_elimination.h"

#include "cinderx/Jit/hir/instr_effects.h"

namespace cinderx::jit::hir {

namespace {

bool isUseful(Instr& instr) {
  return instr.isTerminator() || instr.isSnapshot() || instr.isUseObj() ||
      (instr.asDeoptBase() != nullptr && !instr.isPrimitiveBox()) ||
      (!instr.isPhi() && memoryEffects(instr).may_store != AEmpty);
}

} // namespace

void DeadCodeElimination::Run(Function& func) {
  Worklist<Instr*> worklist;
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      if (isUseful(instr)) {
        worklist.push(&instr);
      }
    }
  }
  std::unordered_set<Instr*> live_set;
  while (!worklist.empty()) {
    auto live_op = worklist.front();
    worklist.pop();
    if (live_set.insert(live_op).second) {
      live_op->visitUses([&](Register*& reg) {
        if (!live_set.contains(reg->instr())) {
          worklist.push(reg->instr());
        }
        return true;
      });
    }
  }
  for (auto& block : func.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      auto& instr = *it;
      ++it;
      if (!live_set.contains(&instr)) {
        instr.unlink();
        delete &instr;
      }
    }
  }
}

} // namespace cinderx::jit::hir
