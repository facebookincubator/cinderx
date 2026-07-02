// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/call_site_live_values.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace cinderx::jit::hir {

namespace {

void addUsesForLiveness(const Instr& instr, RegisterSet& live) {
  if (!instr.isPhi()) {
    instr.visitUses([&](Register* reg) {
      live.insert(reg);
      return true;
    });
  }

  if (instr.numEdges() == 0) {
    return;
  }

  // Phi uses happen at the end of predecessor blocks.
  for (std::size_t i = 0, n = instr.numEdges(); i < n; ++i) {
    BasicBlock* succ = instr.successor(i);
    int phi_idx = -1;
    for (const Instr& succ_instr : *succ) {
      if (!succ_instr.isPhi()) {
        break;
      }
      const Phi& phi = static_cast<const Phi&>(succ_instr);
      if (phi_idx == -1) {
        phi_idx = phi.blockIndex(instr.block());
      }
      live.insert(phi.getOperand(phi_idx));
    }
  }
}

RegisterSet liveBeforeInstr(const Instr& instr, const RegisterSet& live_after) {
  RegisterSet live = live_after;
  if (Register* output = instr.output()) {
    live.erase(output);
  }
  addUsesForLiveness(instr, live);
  return live;
}

void fillCallSiteLiveRegs(
    const RegisterSet& live_regs,
    CallSiteLiveValuesBase& live_values_instr) {
  JIT_CHECK(
      live_values_instr.liveRegs().empty(),
      "Instruction should have no live regs");

  std::vector<Register*> sorted_regs{live_regs.begin(), live_regs.end()};
  std::sort(
      sorted_regs.begin(), sorted_regs.end(), [](Register* a, Register* b) {
        return a->id() < b->id();
      });

  for (Register* reg : sorted_regs) {
    if (reg->type().couldBe(TCPtr)) {
      continue;
    }
    ValueKind value_kind = deoptValueKind(reg->type());
    if (value_kind != ValueKind::kObject) {
      continue;
    }
    live_values_instr.emplaceLiveReg(reg, RefKind::kOwned, value_kind);
  }
  live_values_instr.sortLiveRegs();
}

} // namespace

void CallSiteLiveValues::Run(Function& irfunc) {
  LivenessAnalysis liveness{irfunc};
  liveness.Run();

  for (BasicBlock* block : irfunc.cfg.GetRPOTraversal()) {
    RegisterSet live = liveness.GetOut(block);
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      Instr& instr = *it;
      RegisterSet live_before = liveBeforeInstr(instr, live);
      if (CallSiteLiveValuesBase* live_values =
              instr.asCallSiteLiveValuesBase()) {
        fillCallSiteLiveRegs(live_before, *live_values);
      }
      live = std::move(live_before);
    }
  }
}

} // namespace cinderx::jit::hir
