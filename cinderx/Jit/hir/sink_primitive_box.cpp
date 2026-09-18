// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/sink_primitive_box.h"

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"

#include <unordered_map>
#include <vector>

namespace cinderx::jit::hir {

namespace {

bool isSinkable(const Instr& instr) {
  // MemoryView::readRaw reads a LiveValue as a full 64-bit word and has no
  // record of its width, so a narrower primitive would re-box from unextended
  // bits.
  return instr.isPrimitiveBox() &&
      instr.as<PrimitiveBox>().type() <= (TCDouble | TCInt64 | TCUInt64);
}

// Instructions whose uses only count once something needs their output.  Phi
// and Assign forward whatever they are given, and a sinkable box disappears
// along with its frame state when nothing consumes what it produces.
bool hasConditionalUses(const Instr& instr) {
  return instr.isPhi() || instr.isAssign() || isSinkable(instr);
}

// Collect the registers that must still hold a real PyObject after this pass.
//
// A box whose output is missing from this set is never consumed as a PyObject:
// it exists only so a deopt can hand the boxed value back to the interpreter,
// which the deopt machinery can do from the unboxed value instead.
RegisterSet collectBoxedUses(Function& func) {
  RegisterSet required_objects;
  std::vector<Register*> worklist;
  const RegUses direct_uses = collectDirectRegUses(func);

  auto markBoxed = [&](Register* reg) {
    if (reg != nullptr && reg->type().couldBe(TObject) &&
        required_objects.insert(reg).second) {
      worklist.push_back(reg);
    }
  };

  // Record what `instr` needs as an object, given that it survives the pass.
  auto markRequiredInputs = [&](const Instr& instr) {
    // UseType exists to tell GuardTypeRemoval that a type is relied upon; it
    // is a no-op assertion rather than a consumer of the value.
    if (!instr.isUseType()) {
      for (Register* operand : instr.getOperands()) {
        markBoxed(operand);
      }
    }
    const DeoptBase* deopt = instr.asDeoptBase();
    if (deopt == nullptr) {
      return;
    }
    // The guilty register is reported to the deopt machinery, so it has to
    // survive as a real object.
    markBoxed(deopt->guiltyReg());
    if (FrameState* frame = deopt->frameState()) {
      // Frame state is restore-only, except that no unboxed value can stand in
      // for the unset local a nullable register may be holding.
      frame->visitUses([&](Register* reg) {
        if (reg->type().couldBe(TNullptr)) {
          markBoxed(reg);
        }
        return true;
      });
    }
  };

  for (const auto& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      if (!hasConditionalUses(instr)) {
        markRequiredInputs(instr);
      }
    }
  }
  while (!worklist.empty()) {
    Register* reg = worklist.back();
    worklist.pop_back();
    if (auto it = direct_uses.find(reg); it != direct_uses.end()) {
      for (Instr* user : it->second) {
        if (user->isPhi() || user->isAssign()) {
          markBoxed(user->output());
        }
      }
    }
    const Instr& instr = *reg->instr();
    if (hasConditionalUses(instr)) {
      markRequiredInputs(instr);
    }
  }
  return required_objects;
}

} // namespace

void SinkPrimitiveBox::run(Function& func) {
  RegisterSet required_objects = collectBoxedUses(func);

  // Map each box we can remove to the unboxed value it was built from.
  std::unordered_map<Register*, Register*> sink_map;
  std::vector<Instr*> dead_instrs;
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      if (isSinkable(instr) && !required_objects.contains(instr.output())) {
        sink_map.emplace(instr.output(), instr.as<PrimitiveBox>().value());
        dead_instrs.push_back(&instr);
      }
    }
  }

  if (sink_map.empty()) {
    return;
  }

  // Rewrite the boxes' remaining (frame-state only) uses to the unboxed value.
  // The deopt machinery records the unboxed value's kind and re-boxes it if a
  // deopt fires, so the box is no longer needed on the fast path.
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      // A UseType assertion on a sunk box has nothing left to assert, and
      // rewriting it to a primitive-typed register would be inconsistent.
      if (instr.isUseType() && sink_map.contains(instr.getOperand(0))) {
        dead_instrs.push_back(&instr);
        continue;
      }
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

  // Rewriting inputs can change the type of a Phi or Assign that carried the
  // boxed value through SSA bookkeeping.
  reflowTypes(func);
}

} // namespace cinderx::jit::hir
