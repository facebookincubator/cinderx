// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/postgen.h"

#include "cinderx/Jit/lir/inliner.h"
#include "cinderx/Jit/lir/printer.h"

using namespace jit::codegen;

namespace jit::lir {

namespace {

// Inline C helper functions.
RewriteResult rewriteInlineHelper(function_rewrite_arg_t func) {
  if (!getConfig().lir_opts.inliner) {
    return kUnchanged;
  }

  return LIRInliner::inlineCalls(func) ? kChanged : kUnchanged;
}

// Constant fold unary operations with immediate inputs.
// Negate(Imm(c)) → Move(Imm(-c))
// Invert(Imm(c)) → Move(Imm(~c))
// IntToBool(Imm(c)) → Move(Imm(c ? 1 : 0))
RewriteResult rewriteConstantFoldUnaryOps(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  switch (instr->opcode()) {
    case Instruction::kNegate:
    case Instruction::kInvert:
    case Instruction::kIntToBool:
      break;
    default:
      return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (!input->isImm()) {
    return kUnchanged;
  }

  uint64_t constant = input->getConstant();
  uint64_t result;
  switch (instr->opcode()) {
    case Instruction::kNegate:
      result = static_cast<uint64_t>(-static_cast<int64_t>(constant));
      break;
    case Instruction::kInvert:
      result = ~constant;
      break;
    case Instruction::kIntToBool:
      result = constant ? 1 : 0;
      break;
    default:
      return kUnchanged;
  }

  instr->setOpcode(Instruction::kMove);
  auto dt = input->dataType();
  static_cast<Operand*>(input)->setConstant(result, dt);
  return kChanged;
}

// Fix constant input position. If a binary operation has a constant input,
// always put it as the second operand (or move the 2nd to a register for div
// instructions)
RewriteResult rewriteBinaryOpConstantPosition(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();

  if (instr->isDiv() || instr->isDivUn()) {
    bool changed = false;
    // div/sdiv/udiv don't support immediate operands on AArch64.
    // Input layout is [Imm{0}, dividend, divisor] where input 0 is the x86
    // high-half placeholder. Convert both dividend (input 1) and divisor
    // (input 2) to registers if they are immediates.
    for (int idx = 1; idx <= 2; idx++) {
      auto operand = instr->getInput(idx);
      if (!operand->isImm()) {
        continue;
      }
      auto constant = operand->getConstant();
      auto constant_size = operand->dataType();

      auto move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{constant_size},
          Imm{constant, constant_size});

      instr->setInput(idx, std::make_unique<LinkedOperand>(move));
      changed = true;
    }
    return changed ? kChanged : kUnchanged;
  }

  if (!instr->isAdd() && !instr->isSub() && !instr->isXor() &&
      !instr->isAnd() && !instr->isOr() && !instr->isMul() &&
      !instr->isCompare()) {
    return kUnchanged;
  }

  bool is_commutative_or_compare = !instr->isSub();
  auto input0 = instr->getInput(0);
  auto input1 = instr->getInput(1);

  if (!input0->isImm()) {
    return kUnchanged;
  }

  // TODO: If both are registers we could constant fold here
  if (is_commutative_or_compare && !input1->isImm()) {
    // if the operation is commutative and the second input is not also an
    // immediate, just swap the operands
    if (instr->isCompare()) {
      instr->setOpcode(Instruction::flipComparisonDirection(instr->opcode()));
    }
    auto imm = instr->removeInput(0);
    instr->appendInput(std::move(imm));
    return kChanged;
  }

  // Otherwise, replace the immediate with a new move instruction.
  auto constant = input0->getConstant();
  auto constant_size = input0->dataType();

  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{constant_size},
      Imm{constant, constant_size});
  instr->setInput(0, std::make_unique<LinkedOperand>(move));

  return kChanged;
}

// Rewrite binary instructions with > 32-bit constant.
RewriteResult rewriteBinaryOpLargeConstant(instr_iter_t instr_iter) {
  // rewrite
  //     Vreg2 = BinOp Vreg1, Imm64
  // to
  //     Vreg0 = Mov Imm64
  //     Vreg2 = BinOp Vreg1, VReg0

  Instruction* instr = instr_iter->get();
  if (!instr->isAdd() && !instr->isSub() && !instr->isXor() &&
      !instr->isAnd() && !instr->isOr() && !instr->isMul() &&
      !instr->isCompare()) {
    return kUnchanged;
  }

  // If first operand is an immediate, we need to swap the operands.
  if (instr->getInput(0)->isImm()) {
    // another rewrite will fix this later
    return kUnchanged;
  }

  JIT_CHECK(
      !instr->getInput(0)->isImm(),
      "The first input operand of a binary op instruction should not be "
      "constant");

  auto in1 = instr->getInput(1);
  if (!in1->isImm()) {
    return kUnchanged;
  }

  auto constant = in1->getConstantOrAddress();
#if defined(CINDER_X86_64)
  // All of these instructions support a register operand and a 32-bit immediate
  // operand. None of them support a 64-bit immediate.
  if ((in1->sizeInBits() < 64) || fitsSignedInt<32>(constant)) {
    return kUnchanged;
  }
#elif defined(CINDER_AARCH64)
  if (instr->isAdd() || instr->isSub() || instr->isCompare()) {
    // add, sub, and cmp (which is a pseudo-instruction aliased to subs) all
    // support a 12-bit immediate optionally shifted by 20 bits.
    if (asmjit::arm::Utils::isAddSubImm(constant)) {
      return kUnchanged;
    }
  } else if (instr->isAnd() || instr->isOr() || instr->isXor()) {
    // and, or, and xor use a logical immediate, which is a 13-bit-encoded
    // operand that represents repeated 1 patterns.
    size_t bits = instr->output()->sizeInBits();
    if (asmjit::arm::Utils::isLogicalImm(constant, bits < 32 ? 32 : bits)) {
      return kUnchanged;
    }
  } else {
    // mul has to use registers and does not support immediates.
  }
#else
  CINDER_UNSUPPORTED
#endif

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{in1->dataType()},
      Imm{constant, in1->dataType()});

  // If the first operand is smaller in size than the second operand, replace
  // the first operand with a sign-extended version that matches the size of the
  // second operand.
  if (instr->getInput(0)->sizeInBits() < in1->sizeInBits()) {
    auto movsx = block->allocateInstrBefore(
        instr_iter, Instruction::kMovSX, OutVReg{in1->dataType()});
    movsx->appendInput(instr->releaseInput(0));
    instr->setInput(0, std::make_unique<LinkedOperand>(movsx));
  }

  // Replace the constant with the move.
  instr->setInput(1, std::make_unique<LinkedOperand>(move));

  return kChanged;
}

#if defined(CINDER_X86_64)
// Rewrite storing a large immediate to a memory location in x86-64. Other
// architectures handle this explicitly in the autogen layer.
RewriteResult rewriteMoveToMemoryLargeConstant(instr_iter_t instr_iter) {
  // rewrite
  //     [Vreg0 + offset] = Imm64
  // to
  //     Vreg1 = Mov Imm64
  //     [Vreg0 + offset] = Vreg1

  auto instr = instr_iter->get();
  auto out = instr->output();

  if (!(instr->isMove() || instr->isMoveRelaxed()) || !out->isInd()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (!input->isImm() && !input->isMem()) {
    return kUnchanged;
  }

  auto constant = input->getConstantOrAddress();
  if (fitsSignedInt<32>(constant)) {
    return kUnchanged;
  }

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg(),
      Imm(constant, input->dataType()));

  // Replace the constant input with the move.
  instr->setInput(0, std::make_unique<LinkedOperand>(move));

  return kChanged;
}
#endif

// Most guards involve comparing against a constant immediate. This rewrite
// ensures those immediates fit into comparison instructions (and if they do
// not it splits them).
RewriteResult rewriteGuardLargeConstant(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isGuard()) {
    return kUnchanged;
  }

  constexpr size_t kTargetIndex = 3;
  auto target_opnd = instr->getInput(kTargetIndex);
  if (!target_opnd->isImm() && !target_opnd->isMem()) {
    return kUnchanged;
  }

  auto target_imm = target_opnd->getConstantOrAddress();

#if defined(CINDER_X86_64)
  if (fitsSignedInt<32>(target_imm)) {
    return kUnchanged;
  }
#elif defined(CINDER_AARCH64)
  if (asmjit::arm::Utils::isAddSubImm(target_imm)) {
    return kUnchanged;
  }
#else
  CINDER_UNSUPPORTED
#endif

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg(),
      Imm(target_imm, target_opnd->dataType()));
  instr->setInput(kTargetIndex, std::make_unique<LinkedOperand>(move));
  return kChanged;
}

// Rewrite LoadArg to Bind and allocate a physical register for its input.
RewriteResult rewriteLoadArg(instr_iter_t instr_iter, Environ* env) {
  auto instr = instr_iter->get();
  if (!instr->isLoadArg()) {
    return kUnchanged;
  }
  instr->setOpcode(Instruction::kBind);
  JIT_CHECK(instr->getNumInputs() == 1, "expected one input");
  OperandBase* input = instr->getInput(0);
  JIT_CHECK(input->isImm(), "expected constant arg index as input");
  auto arg_idx = input->getConstant();
  auto loc = env->arg_locations[arg_idx];
  static_cast<Operand*>(input)->setPhyRegOrStackSlot(loc);
  static_cast<Operand*>(input)->setDataType(instr->output()->dataType());
  return kChanged;
}

void populateLoadSecondCallResultPhi(
    DataType data_type,
    Instruction* phi1,
    Instruction* phi2,
    UnorderedMap<Operand*, Instruction*>& seen_srcs);

// Return an Instruction* (which may already exist) defining the second call
// result for src, with the given DataType.
//
// instr, if given, will be reused rather than inserting a new instruction (to
// preserve its vreg identity).
//
// seen_srcs is used to ensure only one Move is inserted for each root Call
// instruction in the presence of loops or repeated Phi uses of the same vreg.
Instruction* getSecondCallResult(
    DataType data_type,
    Operand* src,
    Instruction* instr,
    UnorderedMap<Operand*, Instruction*>& seen_srcs) {
  auto it = seen_srcs.find(src);
  if (it != seen_srcs.end()) {
    return it->second;
  }
  Instruction* src_instr = src->instr();
  BasicBlock* src_block = src_instr->basicblock();
  auto src_it = src_block->iterator_to(src_instr);
  JIT_CHECK(
      src_instr->isCall() || src_instr->isPhi(),
      "LoadSecondCallResult input must come from Call or Phi, not '{}'",
      *src_instr);

  if (src_instr->isCall()) {
    // Check that this Call hasn't already been handled on behalf of another
    // LoadSecondCallResult. If we need to support this pattern in the future,
    // this rewrite function should probably become a standalone pass, with the
    // scope of seen_srcs expanded to the whole function.
    auto next_it = std::next(src_it);
    if (next_it != src_block->instructions().end()) {
      Instruction* next_instr = next_it->get();
      JIT_CHECK(
          !(next_instr->isMove() && next_instr->getNumInputs() == 1 &&
            next_instr->getInput(0)->isReg() &&
            next_instr->getInput(0)->getPhyRegister() == RETURN_REGS[1]),
          "Call output consumed by multiple LoadSecondCallResult instructions");
    }
  }

  if (instr) {
    // We want to keep using the vreg defined by instr, so move it to after
    // src_instr, rather than allocating a new one.
    BasicBlock* instr_block = instr->basicblock();
    auto instr_it = instr_block->iterator_to(instr);
    auto instr_owner = instr_block->removeInstr(instr_it);
    src_block->instructions().insert(std::next(src_it), std::move(instr_owner));
    instr->setNumInputs(0);
  }

  Instruction::Opcode new_op =
      src_instr->isCall() ? Instruction::kMove : Instruction::kPhi;
  if (instr) {
    instr->setOpcode(new_op);
  } else {
    instr = src_block->allocateInstrBefore(
        std::next(src_it), new_op, OutVReg(data_type));
  }
  seen_srcs[src] = instr;
  if (new_op == Instruction::kMove) {
    instr->addOperands(PhyReg(RETURN_REGS[1], data_type));
  } else {
    // instr is now a Phi (either newly-created or a replacement for
    // instr). Recursively populate its inputs with the second result of all
    // original Calls.
    populateLoadSecondCallResultPhi(data_type, src_instr, instr, seen_srcs);
  }

  return instr;
}

// Given a Phi that joins the outputs of multiple Calls (or more Phis that
// ultimately join the outputs of Calls), populate a second, parallel Phi to
// join the second result of all original Calls.
void populateLoadSecondCallResultPhi(
    DataType data_type,
    Instruction* phi1,
    Instruction* phi2,
    UnorderedMap<Operand*, Instruction*>& seen_srcs) {
  for (size_t i = 1; i < phi1->getNumInputs(); i += 2) {
    Operand* src1 = phi1->getInput(i)->getDefine();
    Instruction* instr2 =
        getSecondCallResult(data_type, src1, nullptr, seen_srcs);
    phi2->addOperands(
        Lbl(phi1->getInput(i - 1)->getBasicBlock()), VReg(instr2));
  }
}

// Replace LoadSecondCallResult instructions with an appropriate Move.
RewriteResult rewriteLoadSecondCallResult(instr_iter_t instr_iter) {
  // Replace "%x = LoadSecondCallResult %y" with "%x = Move RDX" immediately
  // after the call that defines %y. If necessary, trace through Phis,
  // inserting multiple Moves and a new Phi to reconcile them.

  Instruction* instr = instr_iter->get();
  if (!instr->isLoadSecondCallResult()) {
    return kUnchanged;
  }

  Operand* src = instr->getInput(0)->getDefine();
  UnorderedMap<Operand*, Instruction*> seen_srcs;
  getSecondCallResult(instr->output()->dataType(), src, instr, seen_srcs);
  return kRemoved;
}

#if defined(CINDER_AARCH64)
// On AArch64, signed operations on sub-32-bit values need sign-extension.
// LIR DataType doesn't track signedness (both cint8 and cuint8 become k8bit),
// so values in registers are zero-extended by default (via ldrb/ldrh/cset).
// For signed comparisons, e.g. cint8 -1 is 0xFF in a register; without
// sign-extension "cmp w0(=255), w1(=1)" with kLT gives false (wrong), but
// with sign-extension "cmp w0(=-1), w1(=1)" with kLT gives true (correct).
// Similarly, signed division (sdiv) needs sign-extended inputs for correctness.
RewriteResult rewriteSignedSubWordOps(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  switch (instr->opcode()) {
    case Instruction::kGreaterThanSigned:
    case Instruction::kGreaterThanEqualSigned:
    case Instruction::kLessThanSigned:
    case Instruction::kLessThanEqualSigned:
    case Instruction::kDiv:
      break;
    default:
      return kUnchanged;
  }

  auto block = instr->basicblock();
  bool changed = false;
  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    auto input = instr->getInput(i);
    if (!input->isReg()) {
      continue;
    }
    auto dt = input->dataType();
    if (dt != OperandBase::k8bit && dt != OperandBase::k16bit) {
      continue;
    }
    auto sext = block->allocateInstrBefore(
        instr_iter, Instruction::kSext, OutVReg{DataType::k32bit});
    sext->appendInput(instr->releaseInput(i));
    instr->setInput(i, std::make_unique<LinkedOperand>(sext));
    changed = true;
  }
  return changed ? kChanged : kUnchanged;
}

// On AArch64, we never are going to produce an output that is less than 32-bits
// for our comparisons so promote all of these to 32-bits so we don't need to
// mask them.
RewriteResult rewritePromoteOutputSize(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  switch (instr->opcode()) {
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
      if (instr->output()->sizeInBits() < 32) {
        instr->output()->setDataType(DataType::k32bit);
        return kChanged;
      }
      return kUnchanged;
    default:
      return kUnchanged;
  }
}

// On AArch64, Guard's kNotZero with a VecD (double) input needs the value
// moved to a GP register first (ARM64 lacks direct FP-register
// test-and-branch). Insert Move(VecD → OutVReg{k64bit}) before the Guard so
// TranslateGuard only sees GP register inputs.
RewriteResult rewriteGuardFPInput(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isGuard()) {
    return kUnchanged;
  }

  constexpr size_t kGuardVarIndex = 2;
  auto guard_var = instr->getInput(kGuardVarIndex);
  if (guard_var->dataType() != DataType::kDouble) {
    return kUnchanged;
  }

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg{DataType::k64bit});
  move->appendInput(instr->releaseInput(kGuardVarIndex));
  instr->setInput(kGuardVarIndex, std::make_unique<LinkedOperand>(move));
  return kChanged;
}

// On AArch64, Guards with kHasType load obj->ob_type into a scratch register
// in TranslateGuard. Decompose this into an explicit Move(Ind) to load the
// type, then convert the guard to kIs so register allocation handles the
// temporary.
//
// Before:  Guard(kHasType, meta, obj, expected_type, ...)
// After:   type_vreg = Move([obj + ob_type_offset])
//          Guard(kIs, meta, type_vreg, expected_type, ...)
RewriteResult rewriteGuardHasType(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isGuard()) {
    return kUnchanged;
  }

  constexpr size_t kKindIndex = 0;
  constexpr size_t kGuardVarIndex = 2;

  auto kind_opnd = instr->getInput(kKindIndex);
  if (kind_opnd->getConstant() != kHasType) {
    return kUnchanged;
  }

  auto guard_var = instr->getInput(kGuardVarIndex);
  JIT_CHECK(guard_var->isLinked(), "Expected guard var to be a linked operand");
  auto guard_var_def = static_cast<LinkedOperand*>(guard_var)->getLinkedInstr();

  auto block = instr->basicblock();
  constexpr int32_t kObTypeOffset = offsetof(PyObject, ob_type);

  // type_vreg = Move([guard_var + ob_type_offset])
  auto type_load = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg{DataType::k64bit});
  type_load->allocateMemoryIndirectInput(guard_var_def, kObTypeOffset);

  // Replace guard var with the loaded type and change kind to kIs.
  instr->setInput(kGuardVarIndex, std::make_unique<LinkedOperand>(type_load));
  static_cast<Operand*>(instr->getInput(kKindIndex))->setConstant(kIs);

  return kChanged;
}

// On AArch64, decompose Lea with MemoryIndirect whose multiplier >= 4 into
// explicit Move(Imm) + MulAdd instructions. Multiplier 0-3 is already optimal
// (add with shifted register in leaIndex), but multiplier >= 4 previously
// required a scratch register in the translate function. This rewrite lets
// register allocation handle the temporary instead.
//
// [base + index * (1 << mult) + offset]  where mult >= 4 becomes:
//   scale = Move(Imm(1 << mult))
//   addr  = MulAdd(index, scale, base)
//   [if offset != 0: addr = Add(addr, Imm(offset))]
//   Lea -> Move(addr)
RewriteResult rewriteLeaLargeMultiplier(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isLea()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (!input->isInd()) {
    return kUnchanged;
  }

  auto ind = input->getMemoryIndirect();
  auto index_op = ind->getIndexRegOperand();
  if (index_op == nullptr) {
    return kUnchanged;
  }

  auto mult = ind->getMultipiler();
  if (mult < 4) {
    return kUnchanged;
  }

  auto block = instr->basicblock();
  auto base_op = ind->getBaseRegOperand();
  auto offset = ind->getOffset();

  // Create a new reference to the same value as an existing operand from
  // inside a MemoryIndirect.
  auto cloneRef = [](OperandBase* op) -> std::unique_ptr<OperandBase> {
    if (op->isLinked()) {
      return std::make_unique<LinkedOperand>(
          static_cast<LinkedOperand*>(op)->getLinkedInstr());
    }
    auto new_op = std::make_unique<Operand>();
    new_op->setPhyRegister(op->getPhyRegister());
    return new_op;
  };

  // scale = Move(Imm(1 << mult))
  auto scale_move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{DataType::k64bit},
      Imm{uint64_t{1} << mult, DataType::k64bit});

  // addr = MulAdd(index, scale, base)  =>  base + index * scale
  auto muladd = block->allocateInstrBefore(
      instr_iter, Instruction::kMulAdd, OutVReg{DataType::k64bit});
  muladd->appendInput(cloneRef(index_op));
  muladd->appendInput(std::make_unique<LinkedOperand>(scale_move));
  muladd->appendInput(cloneRef(base_op));

  Instruction* final_result = muladd;

  if (offset != 0) {
    auto add = block->allocateInstrBefore(
        instr_iter,
        Instruction::kAdd,
        OutVReg{DataType::k64bit},
        VReg{muladd},
        Imm{static_cast<uint64_t>(static_cast<int64_t>(offset)),
            DataType::k64bit});
    final_result = add;
  }

  // Convert Lea to Move(final_result).
  instr->setOpcode(Instruction::kMove);
  instr->setInput(0, std::make_unique<LinkedOperand>(final_result));

  return kChanged;
}

// On AArch64, lower Move/MoveRelaxed with an absolute memory address (kMem)
// input into Move(Imm{addr} → vreg) + Move(Ind{vreg, 0} → output). ARM64
// cannot encode a 64-bit absolute address inline, so translateMove used a
// scratch register. This rewrite lets register allocation handle it instead.
RewriteResult rewriteMoveAbsoluteAddress(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isMove() && !instr->isMoveRelaxed()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (!input->isMem()) {
    return kUnchanged;
  }

  auto block = instr->basicblock();
  auto addr = reinterpret_cast<uint64_t>(input->getMemoryAddress());

  // addr_vreg = Move(Imm{addr})
  auto addr_move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{DataType::k64bit},
      Imm{addr, DataType::k64bit});

  // Replace the Mem input with Ind{addr_vreg, offset=0}. For a simple
  // Ind with no index and offset 0, ptrIndirect resolves to ptr(base)
  // without needing any scratch registers.
  instr->removeInput(0);
  instr->allocateMemoryIndirectInput(
      static_cast<Instruction*>(addr_move), PhyLocation::REG_INVALID, 0, 0);

  return kChanged;
}

// On AArch64, lower stack operands to virtual registers for instructions
// that cannot operate on memory. ARM64 instructions require register operands,
// so this inserts a Move(Stack -> vreg) before the instruction and replaces
// the stack input with a linked reference to the new vreg.
//
// NOT handled here:
//   - Move/MoveRelaxed "Rm": IS the canonical load (the lowering target)
//   - MovZX/MovSX/MovSXD: specialized sign/zero-extending loads from stack
//   - Lea: takes the ADDRESS of a stack slot, not the value
//   - Call: late-created by PostRegAllocRewrite via setOpcode()
//   - EpilogueEnd: special return-value handling
//   - Pop: stack output, not input
RewriteResult rewriteStackInputToVreg(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();

  auto lowerStackInput = [&](size_t idx) -> bool {
    auto input = instr->getInput(idx);
    if (!input->isStack()) {
      return false;
    }
    auto loc = input->getStackSlot();
    auto dt = input->dataType();
    auto move = block->allocateInstrBefore(
        instr_iter, Instruction::kMove, OutVReg{dt}, Stk{loc, dt});
    instr->setInput(idx, std::make_unique<LinkedOperand>(move));
    return true;
  };

  bool changed = false;

  if (instr->isAdd() || instr->isSub() || instr->isXor() || instr->isAnd() ||
      instr->isOr() || instr->isMul() || instr->isCompare()) {
    // Binary ops and compare ops: lower any stack input.
    for (size_t i = 0; i < instr->getNumInputs(); i++) {
      changed |= lowerStackInput(i);
    }
  } else if (instr->isDiv() || instr->isDivUn()) {
    // Div/DivUn may have an Imm{0} prefix for x86 high-half. Lower any
    // non-immediate stack inputs.
    for (size_t i = 0; i < instr->getNumInputs(); i++) {
      changed |= lowerStackInput(i);
    }
  } else {
    switch (instr->opcode()) {
      case Instruction::kNegate:
      case Instruction::kInvert:
        changed |= lowerStackInput(0);
        break;
      case Instruction::kPush:
        changed |= lowerStackInput(0);
        break;
      case Instruction::kInc:
      case Instruction::kDec: {
        // Inc/Dec are read-modify-write: the single operand is both input and
        // output. For a stack operand, insert a load before and a store after:
        //   vreg = Move [stack]
        //   Inc/Dec vreg
        //   Move vreg -> [stack]
        auto input = instr->getInput(0);
        if (input->isStack()) {
          auto loc = input->getStackSlot();
          auto dt = input->dataType();
          auto move = block->allocateInstrBefore(
              instr_iter, Instruction::kMove, OutVReg{dt}, Stk{loc, dt});
          instr->setInput(0, std::make_unique<LinkedOperand>(move));
          auto next_iter = std::next(instr_iter);
          block->allocateInstrBefore(
              next_iter, Instruction::kMove, OutStk{loc, dt}, VReg{move});
          changed = true;
        }
        break;
      }
      case Instruction::kSelect:
        // Select: condition (0), true_val (1), false_val (2)
        for (size_t i = 0; i < instr->getNumInputs(); i++) {
          changed |= lowerStackInput(i);
        }
        break;
      default:
        break;
    }
  }

  return changed ? kChanged : kUnchanged;
}

// On AArch64, lower immediate operands to virtual registers for instructions
// that cannot encode immediates. This inserts a Move(Imm -> vreg) before the
// instruction and replaces the immediate input with a linked reference to the
// new vreg.
//
// NOT handled here (covered elsewhere or not needed):
//   - Binary ops: rewriteBinaryOpLargeConstant
//   - Guard: rewriteGuardLargeConstant
//   - Div/DivUn: rewriteBinaryOpConstantPosition
//   - BitTest input 1: isLogicalImm(1<<n) always encodes
//   - Move "Ri" (load immediate to register): this IS the lowering target
//   - Inc/Dec: hardcoded constant 1, no immediate operand
RewriteResult rewriteNonBinaryImmediateToVreg(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();

  switch (instr->opcode()) {
    case Instruction::kPush: {
      // ARM str can't take an immediate data operand.
      auto input = instr->getInput(0);
      if (!input->isImm()) {
        return kUnchanged;
      }
      auto move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{input->dataType()},
          Imm{input->getConstant(), input->dataType()});
      instr->setInput(0, std::make_unique<LinkedOperand>(move));
      return kChanged;
    }
    case Instruction::kSelect: {
      // ARM csel is register-only; the false_val (input 2) must be a register.
      auto input = instr->getInput(2);
      if (!input->isImm()) {
        return kUnchanged;
      }
      auto move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{input->dataType()},
          Imm{input->getConstant(), input->dataType()});
      instr->setInput(2, std::make_unique<LinkedOperand>(move));
      return kChanged;
    }
    case Instruction::kMove:
    case Instruction::kMoveRelaxed: {
      // Lower immediate input ONLY when the output is memory (Ind or Stack).
      // Do NOT lower "Ri" (register = immediate) — that's the load-immediate
      // instruction and is the target of all other lowerings.
      auto output = instr->output();
      if (!output->isInd() && !output->isStack()) {
        return kUnchanged;
      }
      auto input = instr->getInput(0);
      if (!input->isImm()) {
        return kUnchanged;
      }
      auto move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{input->dataType()},
          Imm{input->getConstant(), input->dataType()});
      instr->setInput(0, std::make_unique<LinkedOperand>(move));
      return kChanged;
    }
    default:
      return kUnchanged;
  }
}

// For Call/VarArgCall/VectorCall instructions with non-register inputs
// (Imm or Stack), insert a Move to load the call target into a vreg so
// translateCall only needs blr(reg).
RewriteResult rewriteCallInput(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isCall() && !instr->isVarArgCall() && !instr->isVectorCall()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (input->isReg() || input->isLinked()) {
    return kUnchanged;
  }

  auto block = instr->basicblock();

  if (input->isImm()) {
    auto move = block->allocateInstrBefore(
        instr_iter,
        Instruction::kMove,
        OutVReg{DataType::k64bit},
        Imm{input->getConstant(), DataType::k64bit});
    instr->setInput(0, std::make_unique<LinkedOperand>(move));
    return kChanged;
  }

  if (input->isStack()) {
    auto loc = input->getStackSlot();
    auto dt = input->dataType();
    auto move = block->allocateInstrBefore(
        instr_iter, Instruction::kMove, OutVReg{dt}, Stk{loc, dt});
    instr->setInput(0, std::make_unique<LinkedOperand>(move));
    return kChanged;
  }

  return kUnchanged;
}
#endif

} // namespace

void PostGenerationRewrite::registerRewrites() {
  // rewriteInlineHelper should occur before other rewrites.
  registerOneRewriteFunction(rewriteInlineHelper, 0);

  registerOneRewriteFunction(rewriteConstantFoldUnaryOps, 1);
  registerOneRewriteFunction(rewriteBinaryOpConstantPosition, 1);
  registerOneRewriteFunction(rewriteBinaryOpLargeConstant, 1);
  registerOneRewriteFunction(rewriteGuardLargeConstant, 1);
  registerOneRewriteFunction(rewriteLoadArg, 1);

#if defined(CINDER_X86_64)
  registerOneRewriteFunction(rewriteMoveToMemoryLargeConstant, 1);
#elif defined(CINDER_AARCH64)
  registerOneRewriteFunction(rewriteSignedSubWordOps, 1);
  registerOneRewriteFunction(rewritePromoteOutputSize, 1);
  registerOneRewriteFunction(rewriteGuardFPInput, 1);
  registerOneRewriteFunction(rewriteGuardHasType, 1);
  registerOneRewriteFunction(rewriteLeaLargeMultiplier, 1);
  registerOneRewriteFunction(rewriteMoveAbsoluteAddress, 1);
  registerOneRewriteFunction(rewriteStackInputToVreg, 1);
  registerOneRewriteFunction(rewriteNonBinaryImmediateToVreg, 1);
  registerOneRewriteFunction(rewriteCallInput, 1);
#endif

  registerOneRewriteFunction(rewriteLoadSecondCallResult, 1);
}

} // namespace jit::lir
