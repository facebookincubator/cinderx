// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/guard_removal.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/printer.h"

#define TRACE(...) JIT_LOGIF(getConfig().log.debug_guard_removal, __VA_ARGS__)

namespace cinderx::jit::hir {

namespace {

// The output type of `instr` if the operand at `operand_index` had type
// `operand_type` instead of its current type.
Type outputTypeWithOperand(
    const Instr& instr,
    std::size_t operand_index,
    Type operand_type) {
  return outputType(instr, [&](std::size_t i) {
    return i == operand_index ? operand_type : instr.getOperand(i)->type();
  });
}

bool guardNeeded(const RegUses& uses, Register* guard_out, Type relaxed_type) {
  // Stores all Register->Type pairs to consider as the algorithm examines
  // whether a guard is needed across passthrough + Phi instructions
  std::queue<std::pair<Register*, Type>> worklist;
  std::unordered_map<Register*, std::unordered_set<Type>> seen_state;
  worklist.emplace(guard_out, relaxed_type);
  seen_state[guard_out].insert(relaxed_type);
  while (!worklist.empty()) {
    auto [reg, reg_type] = worklist.front();
    worklist.pop();
    auto reg_uses = uses.find(reg);
    if (reg_uses == uses.end()) {
      continue;
    }
    for (const Instr* instr : reg_uses->second) {
      for (std::size_t i = 0; i < instr->numOperands(); i++) {
        if (instr->getOperand(i) != reg) {
          continue;
        }
        Register* output = instr->output();
        if (output != nullptr && (instr->isPhi() || isPassthrough(*instr))) {
          Type passthrough_type = outputTypeWithOperand(*instr, i, reg_type);
          if (seen_state[output].insert(passthrough_type).second) {
            worklist.emplace(output, passthrough_type);
          }
        }
        if (output != nullptr && output->isA(TBottom) &&
            !(outputTypeWithOperand(*instr, i, reg_type) <= TBottom)) {
          // This instruction can only fail, and the guard's narrower operand
          // type is what proves it. The code after it has already been
          // replaced with a trap, so widening the type would let the
          // instruction succeed and fall into that trap.
          TRACE("'{}' kept alive by unreachable '{}'", *reg->instr(), *instr);
          return true;
        }
        OperandType expected_type = instr->getOperandType(i);
        // TASK(T106726658): We should be able to remove GuardTypes if we ever
        // add a matching constraint for non-Primitive types, and our GuardType
        // adds an unnecessary refinement. Since we cannot guard on primitive
        // types yet, this should never happen
        if (operandsMustMatch(expected_type)) {
          TRACE("'{}' kept alive by primitive '{}'", *reg->instr(), *instr);
          return true;
        }
        if (!registerTypeMatches(reg_type, expected_type)) {
          TRACE("'{}' kept alive by '{}'", *reg->instr(), *instr);
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace

bool GuardTypeRemoval::run(Function& func) {
  RegUses reg_uses = collectDirectRegUses(func);
  std::vector<std::unique_ptr<Instr>> removed_guards;
  for (auto& block : func.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      auto& instr = *it;
      ++it;

      if (!instr.isGuardType()) {
        continue;
      }

      Register* guard_out = instr.output();
      Register* guard_in = instr.getOperand(0);
      if (!guardNeeded(reg_uses, guard_out, guard_in->type())) {
        auto assign = Assign::create(guard_out, guard_in);
        assign->copyBytecodeOffset(instr);
        instr.replaceWith(*assign);
        removed_guards.emplace_back(&instr);
      }
    }
  }

  bool changed = removed_guards.size() > 0;
  if (changed) {
    CopyPropagation{}.run(func);
    reflowTypes(func);
  }
  return changed;
}

} // namespace cinderx::jit::hir
