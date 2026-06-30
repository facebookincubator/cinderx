// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/target_select.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"

#include <iterator>
#include <memory>
#include <unordered_map>

namespace cinderx::jit::lir {
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

/* AArch64 GPR operations produce at least 32-bit results. Keep semantic
 * sub-32-bit types in generic LIR, then legalize them before register
 * allocation so codegen does not need to mask partial-register results.
 */
void legalizeA64Min32BitOutput(instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  if (instr->output()->sizeInBits() < 32) {
    instr->output()->setDataType(DataType::k32bit);
  }
}

/* AArch64 cannot directly test-and-branch on FP registers. Move double guard
 * inputs through a GP-sized vreg before guard selection and register
 * allocation.
 */
void legalizeA64GuardFPInput(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isGuard(), "Expected Guard, got {}", instr->opname());

  constexpr size_t kGuardVarIndex = 2;
  Operand* guard_var = instr->getInput(kGuardVarIndex);
  if (guard_var->dataType() != DataType::kDouble) {
    return;
  }

  Instruction* move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg{DataType::k64bit});
  move->appendInput(instr->releaseInput(kGuardVarIndex));
  instr->setInput(
      kGuardVarIndex, std::make_unique<Operand>(move, Operand::kLinked));
}

/* Convert from:
 *
 *     addr = Lea [base + index * (1 << mult) + offset]  where mult >= 4
 *
 * to:
 *
 *     scale = Move(Imm(1 << mult))
 *     addr' = MulAdd(index, scale, base)
 *     [if offset != 0: addr' = Add(addr', Imm(offset))]
 *     addr = Move(addr')
 */
void selectA64LeaLargeMultiplier(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isLea(), "Expected Lea, got {}", instr->opname());

  Operand* input = instr->getInput(0);
  if (!input->isInd()) {
    return;
  }

  MemoryIndirect* ind = input->getMemoryIndirect();
  Operand* index_op = ind->getIndexRegOperand();
  if (index_op == nullptr) {
    return;
  }

  uint8_t mult = ind->getMultiplier();
  if (mult < 4) {
    return;
  }

  int32_t offset = ind->getOffset();

  auto ind_input = instr->removeInput(0);
  ind = ind_input->getMemoryIndirect();
  auto index = ind->releaseIndexRegOperand();
  auto base = ind->releaseBaseRegOperand();
  JIT_CHECK(base != nullptr, "Expected Lea with index to also have a base");

  Instruction* scale_move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{DataType::k64bit},
      Imm{uint64_t{1} << mult, DataType::k64bit});

  Instruction* muladd = block->allocateInstrBefore(
      instr_iter, Instruction::kMulAdd, OutVReg{DataType::k64bit});
  muladd->appendInput(std::move(index));
  muladd->appendInput(std::make_unique<Operand>(scale_move, Operand::kLinked));
  muladd->appendInput(std::move(base));

  Instruction* final_result = muladd;
  if (offset != 0) {
    uint64_t offset_value = static_cast<uint64_t>(static_cast<int64_t>(offset));

    Instruction* add = block->allocateInstrBefore(
        instr_iter, Instruction::kAdd, OutVReg{DataType::k64bit}, VReg{muladd});
    if (asmjit::arm::Utils::isAddSubImm(offset_value)) {
      add->addOperands(Imm{offset_value, DataType::k64bit});
    } else {
      Instruction* offset_move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{DataType::k64bit},
          Imm{offset_value, DataType::k64bit});
      add->addOperands(VReg{offset_move});
    }

    final_result = add;
  }

  instr->setOpcode(Instruction::kMove);
  instr->appendInput(std::make_unique<Operand>(final_result, Operand::kLinked));
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

  /* A64GuardCC branches using the condition encoded above. The original Guard
   * variable and target operands are no longer needed. */
  guard->removeInput(3);
  guard->removeInput(2);
}

/* Convert from:
 *
 *     tst w0, w0
 *     b.mi label
 *
 * to:
 *
 *     tbnz w0, #31, label
 */
void selectA64BranchSigned(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* branch = instr_iter->get();
  JIT_DCHECK(
      branch->isBranchS() || branch->isBranchNS(),
      "Expected BranchS or BranchNS, got {}",
      branch->opname());

  /* Find the flag producer that this conditional branch is implicitly relying
   * on. */
  instr_iter_t cursor = instr_iter;
  bool match = false;
  while (cursor != block->instructions().begin()) {
    --cursor;
    Instruction* instr = cursor->get();
    if (InstrProperty::getProperties(instr->opcode()).flag_effects ==
        FlagEffects::kNone) {
      continue;
    }

    if (!instr->isTest32()) {
      return;
    }

    /* Make sure that the branch producer is testing a register against itself
     * so that we can replace with the branch bit set instruction. */
    auto* left = instr->getInput(0);
    auto* right = instr->getInput(1);
    if (!left->isLinked() || !right->isLinked() ||
        left->getLinkedInstr() != right->getLinkedInstr()) {
      return;
    }

    match = true;
    break;
  }

  if (!match) {
    return;
  }

  branch->setOpcode(
      branch->isBranchS() ? Instruction::kBranchBitSet
                          : Instruction::kBranchBitNotSet);

  /* Test the sign bit of the register. */
  auto bit = std::make_unique<Operand>();
  bit->setConstant(31);
  branch->prependInput(std::move(bit));

  /* Take the register that the test is operating on and put it on branch. */
  auto value = cursor->get()->removeInput(0);
  branch->prependInput(std::move(value));

  block->removeInstr(cursor);
}

void selectA64Opcodes(Function* func) {
  UseCounts use_counts = countUses(func);

  for (BasicBlock* block : func->basicblocks()) {
    BasicBlock::InstrList& instrs = block->instructions();

    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;

      switch (cur_iter->get()->opcode()) {
        case Instruction::kEqual:
        case Instruction::kNotEqual:
        case Instruction::kGreaterThanSigned:
        case Instruction::kGreaterThanEqualSigned:
        case Instruction::kLessThanSigned:
        case Instruction::kLessThanEqualSigned:
        case Instruction::kGreaterThanUnsigned:
        case Instruction::kGreaterThanEqualUnsigned:
        case Instruction::kLessThanUnsigned:
        case Instruction::kLessThanEqualUnsigned:
        case Instruction::kAnd:
        case Instruction::kXor:
        case Instruction::kOr:
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Instruction::kLea:
          selectA64LeaLargeMultiplier(block, cur_iter);
          break;
        case Instruction::kCondBranch:
          selectA64CondBranch(block, cur_iter, use_counts);
          break;
        case Instruction::kGuard:
          legalizeA64GuardFPInput(block, cur_iter);
          selectA64Guard(block, cur_iter, use_counts);
          break;
        case Instruction::kBranchS:
        case Instruction::kBranchNS:
          selectA64BranchSigned(block, cur_iter);
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

} // namespace cinderx::jit::lir
