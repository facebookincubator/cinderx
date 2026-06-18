// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/target_select.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"

#include <iterator>
#include <memory>
#include <unordered_map>

namespace jit::lir {
namespace {

#if defined(CINDER_X86_64)
/* x86-64 can materialize an imm64 in a register, but cannot encode an imm64
 * directly as the source of a store to memory. Convert from:
 *
 *     mov [base + offset], imm64
 *
 * to:
 *
 *     movabs tmp, imm64
 *     mov [base + offset], tmp
 */
void selectX64MoveToMemoryLargeConstant(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->isMove() || instr->isMoveRelaxed(),
      "Expected Move or MoveRelaxed, got {}",
      instr->opname());

  Operand* out = instr->output();

  if (!out->isInd()) {
    return;
  }

  Operand* input = instr->getInput(0);
  if (!input->isImm() && !input->isMem()) {
    return;
  }

  uint64_t constant = input->getConstantOrAddress();
  if (fitsSignedInt<32>(constant)) {
    return;
  }

  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg(),
      Imm(constant, input->dataType()));

  instr->setInput(0, std::make_unique<Operand>(move, Operand::kLinked));
}

void selectX64Opcodes(Function* func) {
  for (BasicBlock* block : func->basicblocks()) {
    BasicBlock::InstrList& instrs = block->instructions();
    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;
      switch (cur_iter->get()->opcode()) {
        case Instruction::kMove:
        case Instruction::kMoveRelaxed:
          selectX64MoveToMemoryLargeConstant(block, cur_iter);
          break;
        default:
          break;
      }
    }
  }
}
#elif defined(CINDER_AARCH64)
using UseCounts = std::unordered_map<const Instruction*, size_t>;

void countOperandUse(UseCounts& use_counts, const Operand* operand) {
  if (operand->isLinked()) {
    use_counts[operand->getLinkedInstr()]++;
    return;
  }

  if (!operand->isInd()) {
    return;
  }

  MemoryIndirect* indirect = operand->getMemoryIndirect();
  Operand* base = indirect->getBaseRegOperand();
  if (base->isLinked()) {
    use_counts[base->getLinkedInstr()]++;
  }

  Operand* index = indirect->getIndexRegOperand();
  if (index != nullptr && index->isLinked()) {
    use_counts[index->getLinkedInstr()]++;
  }
}

/* Count the number of uses of each instruction in the function. This
 * information should really be stored in the IR so that we have either use
 * counts or use-def chains, but for now in order to get this functional we're
 * going to count the uses here.
 */
UseCounts countUses(Function* func) {
  UseCounts use_counts;
  for (BasicBlock* block : func->basicblocks()) {
    for (std::unique_ptr<Instruction>& instr : block->instructions()) {
      instr->foreachInputOperand([&use_counts](const Operand* operand) {
        countOperandUse(use_counts, operand);
      });
    }
  }
  return use_counts;
}

/* Check that intervening instructions between two iterator points do not modify
 * flags in any way. This allows the two endpoints to reliably set/get flags. */
bool flagsPreservedBetween(instr_iter_t begin, instr_iter_t end) {
  for (instr_iter_t iter = begin; iter != end; iter++) {
    if (InstrProperty::getProperties(iter->get()->opcode()).flag_effects !=
        FlagEffects::kNone) {
      return false;
    }
  }
  return true;
}

/* Convert from:
 *
 *     cmp x0, x1
 *     cset w2, eq
 *     b.eq label
 *
 * to:
 *
 *     cmp x0, x1
 *     b.eq label
 */
void selectA64CondBranch(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* branch = instr_iter->get();
  JIT_DCHECK(
      branch->isCondBranch(), "Expected CondBranch, got {}", branch->opname());

  /* Check that the input to this conditional branch is not a def. */
  Operand* input = branch->getInput(0);
  if (!input->isLinked()) {
    return;
  }

  /* Check that the input to this conditional branch is a compare in the same
   * block and that it is not used by any other instruction. */
  Instruction* compare = input->getLinkedInstr();
  if (!compare->isCompare() || compare->basicblock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Check that the instructions between the compare and the conditional branch
   * do not modify flags. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!flagsPreservedBetween(std::next(compare_iter), instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Instruction::Opcode branch_opcode =
      Instruction::compareToBranchCC(compare->opcode());

  compare->setOpcode(Instruction::kCmp);
  compare->output()->setNone();
  branch->setOpcode(branch_opcode);
  branch->setNumInputs(0);
}

/* Convert from:
 *
 *     cmp x0, x1
 *     cset w2, lt
 *     cbz w2, deopt
 *
 * to:
 *
 *     cmp x0, x1
 *     b.ge deopt
 */
void selectA64Guard(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* guard = instr_iter->get();
  JIT_DCHECK(guard->isGuard(), "Expected Guard, got {}", guard->opname());

  /* Check that the guard kind is a zero or not zero check. */
  InstrGuardKind kind =
      static_cast<InstrGuardKind>(guard->getInput(0)->getConstant());
  if (kind != InstrGuardKind::kNotZero && kind != InstrGuardKind::kZero) {
    return;
  }

  /* Check that the input to this guard is not a def. */
  Operand* input = guard->getInput(2);
  if (!input->isLinked()) {
    return;
  }

  /* Check that the input to this guard is a compare in the same block and that
   * it is not used by any other instruction. */
  Instruction* compare = input->getLinkedInstr();
  if (!compare->isCompare() || compare->basicblock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Check that the instructions between the compare and the guard do not
   * modify flags. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!flagsPreservedBetween(std::next(compare_iter), instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Instruction::Opcode branch_opcode =
      Instruction::compareToBranchCC(compare->opcode());
  if (kind == InstrGuardKind::kNotZero) {
    branch_opcode = Instruction::negateBranchCC(branch_opcode);
  }

  compare->setOpcode(Instruction::kCmp);
  compare->output()->setNone();

  guard->setOpcode(Instruction::kA64GuardCC);
  guard->getInput(0)->setConstant(static_cast<uint64_t>(branch_opcode));
  // A64GuardCC branches using the condition encoded above. The original Guard
  // variable and target operands are no longer needed.
  guard->removeInput(3);
  guard->removeInput(2);
}

void selectA64Opcodes(Function* func) {
  UseCounts use_counts = countUses(func);
  for (BasicBlock* block : func->basicblocks()) {
    BasicBlock::InstrList& instrs = block->instructions();
    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;
      switch (cur_iter->get()->opcode()) {
        case Instruction::kCondBranch:
          selectA64CondBranch(block, cur_iter, use_counts);
          break;
        case Instruction::kGuard:
          selectA64Guard(block, cur_iter, use_counts);
          break;
        default:
          break;
      }
    }
  }
}
#else
void selectUnknownTargetOpcodes(Function* func) {
  (void)func;
}
#endif

} // namespace

void selectTargetOpcodes(Function* func) {
#if defined(CINDER_X86_64)
  selectX64Opcodes(func);
#elif defined(CINDER_AARCH64)
  selectA64Opcodes(func);
#else
  selectUnknownTargetOpcodes(func);
#endif
}

} // namespace jit::lir
