// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/materialize_steals.h"

#include "cinderx/Jit/hir/instr_effects.h"

namespace cinderx::jit::hir {

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
          instr.isPhi() || instr.numEdges() > 0 ||
          // BatchDecref does not correctly handle effects.stolen_inputs and
          // doesn't steal anyway.
          instr.isBatchDecref()) {
        continue;
      }

      MemoryEffects effects = memoryEffects(instr);
      if (effects.stolen_inputs.getPopCount() == 0) {
        continue;
      }

      auto cursor = block.iterator_to(instr);
      for (int i = 0; i < effects.stolen_inputs.getNumBits(); ++i) {
        if (!effects.stolen_inputs.getBit(i)) {
          continue;
        }

        Register* operand = instr.getOperand(i);
        if (!operand->type().couldBe(TOptObject) ||
            operand->instr()->isMaterializeRef()) {
          continue;
        }

        Register* output = func.env.allocateRegister();
        MaterializeRef* materialized = MaterializeRef::create(output, operand);
        materialized->copyBytecodeOffset(instr);
        output->setType(outputType(*materialized));
        block.insert(materialized, cursor);
        instr.setOperand(i, output);
      }
    }
  }
}

} // namespace cinderx::jit::hir
