// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/sink_primitive_box.h"

#include "cinderx/Jit/hir/hir.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cinderx::jit::hir {

void SinkPrimitiveBox::run(Function& func) {
  // Registers that escape as a real PyObject, i.e. appear as a data operand of
  // some consuming instruction.  UseType is excluded: it is a no-op type
  // assertion (it keeps a GuardType alive), not a real consumer, so a box used
  // only by UseType and deopt frame state does not actually escape.
  std::unordered_set<Register*> escapes;
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      if (instr.isUseType()) {
        continue;
      }
      for (std::size_t i = 0, n = instr.numOperands(); i < n; ++i) {
        escapes.insert(instr.getOperand(i));
      }
    }
  }

  // Map each sinkable box's result to its unboxed source value, remember the
  // box instructions, and collect the UseType assertions on those boxes (which
  // become meaningless once the box is gone).
  std::unordered_map<Register*, Register*> sink_map;
  std::vector<Instr*> dead_instrs;
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      if (!instr.isPrimitiveBox()) {
        continue;
      }
      auto& box = static_cast<PrimitiveBox&>(instr);
      // Limited to floats for now: deopt re-boxes a CDouble via PyFloat.
      if (!(box.type() <= TCDouble)) {
        continue;
      }
      if (!escapes.contains(box.output())) {
        sink_map.emplace(box.output(), box.value());
        dead_instrs.push_back(&instr);
      }
    }
  }

  if (sink_map.empty()) {
    return;
  }

  // Drop UseType assertions on sunk boxes before rewriting, so we don't rewrite
  // them to a primitive-typed register (which would be type-inconsistent).
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      if (instr.isUseType() && sink_map.contains(instr.getOperand(0))) {
        dead_instrs.push_back(&instr);
      }
    }
  }

  // Rewrite the boxes' remaining (frame-state only) uses to the unboxed value.
  // The deopt machinery records the unboxed value's kind and re-boxes it if a
  // deopt fires, so the box is no longer needed on the fast path.
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      instr.visitUses([&](Register*& reg) {
        auto it = sink_map.find(reg);
        if (it != sink_map.end()) {
          reg = it->second;
        }
        return true;
      });
    }
  }

  // The boxes (and their UseType assertions) now have no uses.
  for (Instr* instr : dead_instrs) {
    instr->unlink();
    delete instr;
  }
}

} // namespace cinderx::jit::hir
