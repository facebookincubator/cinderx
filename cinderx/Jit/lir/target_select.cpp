// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/target_select.h"

#include "cinderx/Common/define.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"

#include <algorithm>
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
void selectX64StoreLargeConstant(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isStore(), "Expected Store, got {}", instr->opname());

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
      instr_iter, Opcode::kMove, OutVReg(), Imm(constant, input->dataType()));

  instr->setInput(0, std::make_unique<Operand>(move, Operand::kLinked));
}

/* x86-64 Select lowers to mov+test+cmov. The true value (input 1) feeds
 * cmov and must be in a register, while the false value (input 2) can be
 * an immediate encoded in the mov. Materialize an immediate true value:
 *
 *     Select(cond, Imm(true), false_val)
 *
 * to:
 *
 *     tmp = Move(Imm(true))
 *     Select(cond, tmp, false_val)
 */
void selectX64SelectTrueImmediate(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isSelect(), "Expected Select, got {}", instr->opname());

  Operand* input = instr->getInput(1);
  if (!input->isImm()) {
    return;
  }

  auto move = block->allocateInstrBefore(
      instr_iter,
      Opcode::kMove,
      OutVReg(input->dataType()),
      Imm(input->getConstant(), input->dataType()));

  instr->setInput(1, std::make_unique<Operand>(move, Operand::kLinked));
}

void selectX64Opcodes(Function* func) {
  for (BasicBlock* block : func->basicBlocks()) {
    BasicBlock::InstrList& instrs = block->instructions();
    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;
      switch (cur_iter->get()->opcode()) {
        case Opcode::kStore:
          selectX64StoreLargeConstant(block, cur_iter);
          break;
        case Opcode::kSelect:
          selectX64SelectTrueImmediate(block, cur_iter);
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
  for (BasicBlock* block : func->basicBlocks()) {
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
    if (writesFlags(iter->get()->opcode())) {
      return false;
    }
  }
  return true;
}

/* Ensure the flags set by a comparison reach its consumer. If an intervening
 * instruction overwrites them, move the comparison immediately before the
 * consumer when possible. */
bool makeCompareFlagsAvailable(
    BasicBlock* block,
    instr_iter_t compare_iter,
    instr_iter_t consumer_iter) {
  if (flagsPreservedBetween(std::next(compare_iter), consumer_iter)) {
    return true;
  }

  Instruction* compare = compare_iter->get();
  for (size_t idx = 0; idx < compare->getNumInputs(); idx++) {
    const Operand* operand = compare->getInput(idx);

    /* Floating-point comparisons can update floating-point exception state, so
     * moving them may change observable behavior. */
    if (operand->isFp()) {
      return false;
    }

    /* Linked operands retain their values when their live ranges are extended,
     * and immediates are immutable. Other operands may target mutable
     * locations, so moving them would have observable effects. */
    if (!operand->isLinked() && !operand->isImm()) {
      return false;
    }
  }

  /* Move the comparison immediately before its consumer. */
  block->instructions().splice(
      consumer_iter, block->instructions(), compare_iter);
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

/* AArch64 signed operations on sub-32-bit values need sign-extension. LIR
 * DataType doesn't track signedness, so values in registers are zero-extended
 * by default. Signed comparisons and signed division need explicit 32-bit
 * signed inputs for correctness.
 */
void legalizeA64SignedSubWordInputs(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      (instr->isCompare() && isSignedCompare(instr->condition())) ||
          instr->opcode() == Opcode::kDiv,
      "Expected signed comparison or Div, got {}",
      instr->opname());

  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    Operand* input = instr->getInput(i);
    if (!input->isVreg() && !input->isReg()) {
      continue;
    }

    DataType dt = input->dataType();
    if (dt != Operand::k8bit && dt != Operand::k16bit) {
      continue;
    }

    Instruction* sext = block->allocateInstrBefore(
        instr_iter, Opcode::kSext, OutVReg{DataType::k32bit});
    sext->appendInput(instr->releaseInput(i));
    instr->setInput(i, std::make_unique<Operand>(sext, Operand::kLinked));
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
      instr_iter, Opcode::kMove, OutVReg{DataType::k64bit});
  move->appendInput(instr->releaseInput(kGuardVarIndex));
  instr->setInput(
      kGuardVarIndex, std::make_unique<Operand>(move, Operand::kLinked));
}

Instruction* moveA64StackInputToVreg(
    BasicBlock* block,
    instr_iter_t instr_iter,
    size_t idx) {
  Instruction* instr = instr_iter->get();
  Operand* input = instr->getInput(idx);
  JIT_DCHECK(input->isStack(), "Expected stack input");

  PhyLocation loc = input->getStackSlot();
  DataType dt = input->dataType();
  Instruction* move = block->allocateInstrBefore(
      instr_iter, Opcode::kLoad, OutVReg{dt}, Stk{loc, dt});
  instr->setInput(idx, std::make_unique<Operand>(move, Operand::kLinked));
  return move;
}

/* AArch64 unary arithmetic instructions only operate on registers. */
void legalizeA64UnaryStackInput(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->isNegate() || instr->isInvert(),
      "Expected Negate or Invert, got {}",
      instr->opname());

  if (!instr->getInput(0)->isStack()) {
    return;
  }

  moveA64StackInputToVreg(block, instr_iter, 0);
}

/* AArch64 Select lowers to register-only csel. */
void legalizeA64SelectStackInputs(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isSelect(), "Expected Select, got {}", instr->opname());

  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    if (instr->getInput(i)->isStack()) {
      moveA64StackInputToVreg(block, instr_iter, i);
    }
  }
}

/* AArch64 Select lowers to register-only csel, so immediate true/false
 * values must be materialized into registers:
 *
 *     Select(cond, Imm(true), Imm(false))
 *
 * to:
 *
 *     tmp_true = Move(Imm(true))
 *     tmp_false = Move(Imm(false))
 *     Select(cond, tmp_true, tmp_false)
 */
void legalizeA64SelectImmediateInputs(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isSelect(), "Expected Select, got {}", instr->opname());

  for (size_t i = 1; i <= 2; i++) {
    Operand* input = instr->getInput(i);
    if (!input->isImm()) {
      continue;
    }
    Instruction* move = block->allocateInstrBefore(
        instr_iter,
        Opcode::kMove,
        OutVReg(input->dataType()),
        Imm(input->getConstant(), input->dataType()));
    instr->setInput(i, std::make_unique<Operand>(move, Operand::kLinked));
  }
}

/* AArch64 Inc/Dec only operate on registers. Rewrite stack updates through a
 * virtual register so register allocation handles the temporary.
 */
void legalizeA64StackInputForIncDec(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->isInc() || instr->isDec(),
      "Expected Inc or Dec, got {}",
      instr->opname());

  Operand* input = instr->getInput(0);
  if (!input->isStack()) {
    return;
  }

  PhyLocation loc = input->getStackSlot();
  DataType dt = input->dataType();
  Instruction* move = moveA64StackInputToVreg(block, instr_iter, 0);

  block->allocateInstrBefore(
      std::next(instr_iter), Opcode::kStore, OutStk{loc, dt}, VReg{move});
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
      Opcode::kMove,
      OutVReg{DataType::k64bit},
      Imm{uint64_t{1} << mult, DataType::k64bit});

  Instruction* muladd = block->allocateInstrBefore(
      instr_iter, Opcode::kMulAdd, OutVReg{DataType::k64bit});
  muladd->appendInput(std::move(index));
  muladd->appendInput(std::make_unique<Operand>(scale_move, Operand::kLinked));
  muladd->appendInput(std::move(base));

  Instruction* final_result = muladd;
  if (offset != 0) {
    uint64_t offset_value = static_cast<uint64_t>(static_cast<int64_t>(offset));

    Instruction* add = block->allocateInstrBefore(
        instr_iter, Opcode::kAdd, OutVReg{DataType::k64bit}, VReg{muladd});
    if (asmjit::arm::Utils::isAddSubImm(offset_value)) {
      add->addOperands(Imm{offset_value, DataType::k64bit});
    } else {
      Instruction* offset_move = block->allocateInstrBefore(
          instr_iter,
          Opcode::kMove,
          OutVReg{DataType::k64bit},
          Imm{offset_value, DataType::k64bit});
      add->addOperands(VReg{offset_move});
    }

    final_result = add;
  }

  instr->setOpcode(Opcode::kMove);
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
  if (!isCompare(compare->opcode()) || compare->basicBlock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Make the compare's flags available to the conditional branch. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!makeCompareFlagsAvailable(block, compare_iter, instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Condition cond = compare->condition();

  compare->setOpcode(Opcode::kCmp);
  compare->output()->setNone();
  branch->setOpcode(Opcode::kBranchCC);
  branch->setCondition(cond);
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
  if (!isCompare(compare->opcode()) || compare->basicBlock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Make the compare's flags available to the guard. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!makeCompareFlagsAvailable(block, compare_iter, instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Condition cond = compare->condition();
  if (kind == InstrGuardKind::kNotZero) {
    cond = negate(cond);
  }

  compare->setOpcode(Opcode::kCmp);
  compare->output()->setNone();

  guard->setOpcode(Opcode::kA64GuardCC);
  guard->getInput(0)->setConstant(static_cast<uint64_t>(cond));

  /* A64GuardCC branches using the condition encoded above. The original Guard
   * variable and target operands are no longer needed. */
  guard->removeInput(3);
  guard->removeInput(2);
}

/* Convert from:
 *
 *     cmp x0, x1
 *     cset w2, cc
 *     cmp w2, 0
 *     csel x3, x4, x5, ne
 *
 * to:
 *
 *     cmp x0, x1
 *     csel x3, x4, x5, cc
 */
void selectA64Select(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* select = instr_iter->get();
  JIT_DCHECK(select->isSelect(), "Expected Select, got {}", select->opname());

  Operand* input = select->getInput(0);
  if (!input->isLinked()) {
    return;
  }

  Instruction* compare = input->getLinkedInstr();
  if (!isCompare(compare->opcode()) || compare->basicBlock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!makeCompareFlagsAvailable(block, compare_iter, instr_iter)) {
    return;
  }

  Condition cond = compare->condition();
  compare->setOpcode(Opcode::kCmp);
  compare->output()->setNone();

  select->setOpcode(Opcode::kA64SelectCC);
  select->getInput(0)->setConstant(static_cast<uint64_t>(cond));
}

/* Whether an AArch64 mov of `value` (at `bits` width) assembles to a single
 * instruction. Mirrors asmjit's Mov expansion: a single movz/movn when the
 * value has one significant 16-bit halfword, otherwise a single orr when the
 * value is a logical immediate. Anything else needs a movz/movk sequence. */
bool fitsInSingleMovA64(uint64_t value, unsigned bits) {
  if (bits == 32) {
    uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF0000u) == 0 || (v & 0xFFFF0000u) == 0xFFFF0000u ||
        (v & 0x0000FFFFu) == 0 || (v & 0x0000FFFFu) == 0x0000FFFFu) {
      return true;
    }
    return asmjit::arm::Utils::isLogicalImm(v, 32);
  }
  if (value <= 0xFFFFFFFFu) {
    uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF0000u) == 0 || (v & 0xFFFF0000u) == 0xFFFF0000u ||
        (v & 0x0000FFFFu) == 0 || (v & 0x0000FFFFu) == 0x0000FFFFu) {
      return true;
    }
  } else {
    int non_zero = 0;
    int all_ones = 0;
    for (int i = 0; i < 4; i++) {
      uint16_t hw = (value >> (i * 16)) & 0xFFFF;
      if (hw != 0) {
        non_zero++;
      }
      if (hw == 0xFFFF) {
        all_ones++;
      }
    }
    int movn_count = (all_ones == 4) ? 1 : (4 - all_ones);
    if (std::min(non_zero, movn_count) == 1) {
      return true;
    }
  }
  return asmjit::arm::Utils::isLogicalImm(value, 64);
}

/* Convert a select between two immediate constants from:
 *
 *     Select d, c, C1, C2
 *
 * to:
 *
 *     Move t, C2
 *     Xor t2, t, #(C1 ^ C2)
 *     Select d, c, t2, t
 *
 * Materializing both constants can cost two mov/movk sequences (notably for
 * addresses like the immortal Py_True/Py_False singletons). When the xor
 * difference is encodable as a logical immediate, flipping it with one eor
 * saves an instruction. The transform only applies when both constants need
 * more than a single mov, so the eor never costs more than the move it
 * replaces. It runs before immediate legalization, so remaining immediates
 * are still materialized when it does not apply.
 */
void selectA64SelectXor(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* select = instr_iter->get();
  JIT_DCHECK(select->isSelect(), "Expected Select, got {}", select->opname());

  Operand* in1 = select->getInput(1);
  Operand* in2 = select->getInput(2);
  if (!in1->isImm() || !in2->isImm()) {
    return;
  }
  DataType dtype = in1->dataType();
  if (dtype != in2->dataType() || dtype == DataType::kDouble) {
    return;
  }
  size_t bits = bitSize(dtype);
  if (bits != 32 && bits != 64) {
    return;
  }

  uint64_t v1 = in1->getConstant();
  uint64_t v2 = in2->getConstant();
  if (bits == 32) {
    v1 &= 0xFFFFFFFFu;
    v2 &= 0xFFFFFFFFu;
  }
  if (v1 == v2) {
    return;
  }
  if (fitsInSingleMovA64(v1, bits) || fitsInSingleMovA64(v2, bits)) {
    return;
  }
  uint64_t diff = v1 ^ v2;
  if (!asmjit::arm::Utils::isLogicalImm(diff, bits)) {
    return;
  }

  // The input-2 value becomes the shared base; xor-ing the difference
  // reconstructs input 1.
  DataType int_dtype = bits == 32 ? DataType::k32bit : DataType::k64bit;
  Instruction* base_move = block->allocateInstrBefore(
      instr_iter, Opcode::kMove, OutVReg{dtype}, Imm{v2, dtype});
  Instruction* eor = block->allocateInstrBefore(
      instr_iter,
      Opcode::kXor,
      OutVReg{dtype},
      VReg{base_move},
      Imm{diff, int_dtype});
  select->setInput(1, std::make_unique<Operand>(eor, Operand::kLinked));
  select->setInput(2, std::make_unique<Operand>(base_move, Operand::kLinked));
}

bool readsFlags(Opcode opcode) {
  return opcode == Opcode::kBranchCC || opcode == Opcode::kA64GuardCC ||
      opcode == Opcode::kA64SelectCC;
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
      branch->isBranchCC(), "Expected BranchCC, got {}", branch->opname());

  /* Only a branch on the sign flag can become a test-bit-and-branch. */
  Condition cond = branch->condition();
  if (cond != Condition::kSign && cond != Condition::kNotSign) {
    return;
  }

  /* Find the flag producer that this conditional branch is implicitly relying
   * on. */
  instr_iter_t cursor = instr_iter;
  bool match = false;
  while (cursor != block->instructions().begin()) {
    --cursor;
    Instruction* instr = cursor->get();
    if (!writesFlags(instr->opcode())) {
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
      cond == Condition::kSign ? Opcode::kBranchBitSet
                               : Opcode::kBranchBitNotSet);

  /* Test the sign bit of the register. */
  auto bit = std::make_unique<Operand>();
  bit->setConstant(31);
  branch->prependInput(std::move(bit));

  /* Take the register that the test is operating on and put it on branch. */
  auto value = cursor->get()->removeInput(0);
  branch->prependInput(std::move(value));

  block->removeInstr(cursor);
}

/* Convert:
 *     tst x0, x0
 *     b.eq/b.ne label
 * to:
 *     cbz/cbnz x0, label
 */
bool selectA64BranchZero(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* branch = instr_iter->get();
  JIT_DCHECK(
      branch->isBranchCC(), "Expected BranchCC, got {}", branch->opname());

  Condition cond = branch->condition();
  bool branches_on_zero = cond == Condition::kZero || cond == Condition::kEqual;
  bool branches_on_nonzero =
      cond == Condition::kNotZero || cond == Condition::kNotEqual;
  if (!branches_on_zero && !branches_on_nonzero) {
    return false;
  }

  // We need an adjacent test instruction before the branch.
  if (instr_iter == block->instructions().begin()) {
    return false;
  }
  auto test_iter = std::prev(instr_iter);
  Instruction* test_instr = test_iter->get();
  if (!test_instr->isTest() && !test_instr->isTest32()) {
    return false;
  }

  // Continue only when both sides use the same register.
  auto* left = test_instr->getInput(0);
  auto* right = test_instr->getInput(1);
  if (!left->isLinked() || !right->isLinked() ||
      left->getLinkedInstr() != right->getLinkedInstr()) {
    return false;
  }

  DataType type = left->dataType();
  if (test_instr->isTest32() && type != DataType::k32bit) {
    return false;
  }
  if (type != DataType::k32bit && type != DataType::k64bit &&
      type != DataType::kObject && type != DataType::kObjectUntagged) {
    return false;
  }

  // Don't remove the test if its flags are used later.
  if (std::any_of(
          std::next(instr_iter),
          block->instructions().end(),
          [](auto const& instr) { return readsFlags(instr->opcode()); })) {
    return false;
  }

  branch->setOpcode(
      branches_on_zero ? Opcode::kCmpBranchZero : Opcode::kCmpBranchNonZero);
  branch->prependInput(test_instr->removeInput(0));
  block->removeInstr(test_iter);
  return true;
}

void selectA64Opcodes(Function* func) {
  UseCounts use_counts = countUses(func);

  for (BasicBlock* block : func->basicBlocks()) {
    BasicBlock::InstrList& instrs = block->instructions();

    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;

      switch (cur_iter->get()->opcode()) {
        case Opcode::kCompare:
          if (isSignedCompare(cur_iter->get()->condition())) {
            legalizeA64SignedSubWordInputs(block, cur_iter);
          }
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Opcode::kAnd:
        case Opcode::kXor:
        case Opcode::kOr:
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Opcode::kDiv:
          legalizeA64SignedSubWordInputs(block, cur_iter);
          break;
        case Opcode::kLea:
          selectA64LeaLargeMultiplier(block, cur_iter);
          break;
        case Opcode::kCondBranch:
          selectA64CondBranch(block, cur_iter, use_counts);
          break;
        case Opcode::kGuard:
          legalizeA64GuardFPInput(block, cur_iter);
          selectA64Guard(block, cur_iter, use_counts);
          break;
        case Opcode::kNegate:
        case Opcode::kInvert:
          legalizeA64UnaryStackInput(block, cur_iter);
          break;
        case Opcode::kSelect:
          legalizeA64SelectStackInputs(block, cur_iter);
          selectA64SelectXor(block, cur_iter);
          legalizeA64SelectImmediateInputs(block, cur_iter);
          selectA64Select(block, cur_iter, use_counts);
          break;
        case Opcode::kInc:
        case Opcode::kDec:
          legalizeA64StackInputForIncDec(block, cur_iter);
          break;
        case Opcode::kBranchCC:
          if (!selectA64BranchZero(block, cur_iter)) {
            selectA64BranchSigned(block, cur_iter);
          }
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
