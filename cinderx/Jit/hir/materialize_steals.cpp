// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/materialize_steals.h"

#include "cinderx/Jit/hir/instr_effects.h"

namespace jit::hir {

// Insert MaterializeRef before each stolen input so that the actual incref
// happens before ownership is transferred. The refcount insertion pass adds
// increfs for values that will be stolen, but those increfs are no-ops for
// deferred-RC tagged pointers. MaterializeRef strips the tag and performs a
// real incref when needed, ensuring the stolen reference is properly owned.
void MaterializeSteals::Run(Function& func) {
  for (auto& block : func.cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      Instr& instr = *it;

      if (
          // Memory effects are undefined for these cases.
          instr.IsPhi() || instr.numEdges() > 0 ||
          // BatchDecref does not correctly handle effects.stolen_inputs and
          // doesn't steal anyway.
          instr.IsBatchDecref()) {
        continue;
      }

      MemoryEffects effects = memoryEffects(instr);
      if (effects.stolen_inputs.GetPopCount() == 0) {
        continue;
      }

      auto cursor = block.iterator_to(instr);
      for (int i = 0; i < effects.stolen_inputs.GetNumBits(); ++i) {
        if (!effects.stolen_inputs.GetBit(i)) {
          continue;
        }

        Register* operand = instr.GetOperand(i);
        if (!operand->type().couldBe(TOptObject) ||
            operand->instr()->IsMaterializeRef()) {
          continue;
        }

        Register* output = func.env.AllocateRegister();
        MaterializeRef* materialized = MaterializeRef::create(output, operand);
        materialized->copyBytecodeOffset(instr);
        output->set_type(outputType(*materialized));
        block.insert(materialized, cursor);
        instr.SetOperand(i, output);
      }
    }
  }
}

} // namespace jit::hir
