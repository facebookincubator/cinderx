// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/postalloc.h"

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/printer.h"

#include <optional>

using namespace jit::codegen;

namespace jit::lir {

namespace {

RewriteResult removePhiInstructions(instr_iter_t instr_iter) {
  auto& instr = *instr_iter;

  if (instr->opcode() == Instruction::kPhi) {
    auto block = instr->basicblock();
    block->removeInstr(instr_iter);
    return kRemoved;
  }

  return kUnchanged;
}

// Insert a move from an operand to a memory location given by base + index.
// This function handles cases where operand is a >32-bit immediate and operand
// is a stack location.
void insertMoveToMemoryLocation(
    BasicBlock* block,
    instr_iter_t instr_iter,
    PhyLocation base,
    int index,
    const OperandBase* operand,
    PhyLocation temp = arch::reg_scratch_0_loc) {
  auto data_type = operand->dataType();

  if (operand->isImm()) {
    auto constant = operand->getConstant();
    if (
#if defined(CINDER_X86_64)
        !fitsSignedInt<32>(constant) ||
#endif
        operand->isFp()) {
      block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutPhyReg{temp, data_type},
          Imm{constant, data_type});
      block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutInd{base, index, data_type},
          PhyReg{temp, data_type});
    } else {
      block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutInd{base, index, data_type},
          Imm{constant, data_type});
    }
    return;
  }

  if (operand->isReg()) {
    PhyLocation loc = operand->getPhyRegister();
    block->allocateInstrBefore(
        instr_iter,
        Instruction::kMove,
        OutInd{base, index, data_type},
        PhyReg{loc});
    return;
  }

  PhyLocation loc = operand->getStackSlot();
  block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutPhyReg{temp, data_type}, Stk{loc});
  block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutInd{base, index, data_type},
      PhyReg{temp, data_type});
}

int rewriteRegularFunction(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();

  auto num_inputs = instr->getNumInputs();
  size_t arg_reg = 0;
  size_t fp_arg_reg = 0;
  int stack_arg_size = 0;

  for (size_t i = 1; i < num_inputs; i++) {
    auto operand = instr->getInput(i);
    bool operand_imm = operand->isImm();

    if (operand->isFp()) {
      if (fp_arg_reg < FP_ARGUMENT_REGS.size()) {
        if (operand_imm) {
          block->allocateInstrBefore(
              instr_iter,
              Instruction::kMove,
              OutPhyReg(arch::reg_scratch_0_loc),
              Imm(operand->getConstant()));
        }
        auto move = block->allocateInstrBefore(instr_iter, Instruction::kMove);
        move->output()->setPhyRegister(FP_ARGUMENT_REGS[fp_arg_reg++]);
        move->output()->setDataType(OperandBase::kDouble);

        if (operand_imm) {
          move->allocatePhyRegisterInput(arch::reg_scratch_0_loc);
        } else {
          move->appendInput(instr->releaseInput(i));
        }
      } else {
        insertMoveToMemoryLocation(
            block,
            instr_iter,
            arch::reg_stack_pointer_loc,
            stack_arg_size,
            operand);
        stack_arg_size += sizeof(void*);
      }
      continue;
    }

    if (arg_reg < ARGUMENT_REGS.size()) {
      auto move = block->allocateInstrBefore(instr_iter, Instruction::kMove);
      move->output()->setPhyRegister(ARGUMENT_REGS[arg_reg++]);
      move->output()->setDataType(operand->dataType());
      move->appendInput(instr->releaseInput(i));
    } else {
      insertMoveToMemoryLocation(
          block,
          instr_iter,
          arch::reg_stack_pointer_loc,
          stack_arg_size,
          operand);
      stack_arg_size += sizeof(void*);
    }
  }

  return stack_arg_size;
}

int prepareArgsArray(
    instr_iter_t instr_iter,
    size_t num_args,
    size_t flags,
    size_t first_arg,
    PhyLocation dest,
    PhyLocation size_dest) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();
  constexpr size_t PTR_SIZE = sizeof(void*);

  // offset on the stack where arg reservation starts...
  const int kVectorcallArgsOffset = 1;
  auto num_allocs = num_args + kVectorcallArgsOffset;
  int rsp_sub = ((num_allocs % 2) ? num_allocs + 1 : num_allocs) * PTR_SIZE;

  // lea dest, [sp + kVectorcallArgsOffset * PTR_SIZE]
  block->allocateInstrBefore(
      instr_iter,
      Instruction::kLea,
      OutPhyReg(dest),
      Ind(arch::reg_stack_pointer_loc, kVectorcallArgsOffset * PTR_SIZE));

  // mov arg2, num_args
  block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutPhyReg(size_dest, lir::OperandBase::k64bit),
      Imm(num_args | flags, lir::OperandBase::k64bit));

  for (size_t i = first_arg; i < first_arg + num_args; i++) {
    auto arg = instr->getInput(i);
    int arg_offset = (i - first_arg) * PTR_SIZE;
    insertMoveToMemoryLocation(block, instr_iter, dest, arg_offset, arg);
  }
  return rsp_sub;
}

int rewriteVectorCallFunctions(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  // For vector calls there are 4 fixed arguments:
  // * #0   - runtime helper function
  // * #1   - flags to be added to nargsf
  // * #2   - callable
  // * #n-1 - kwnames
  constexpr int kFirstArg = 3;

  auto flag = instr->getInput(1)->getConstant();
  auto num_args = instr->getNumInputs() - kFirstArg - 1;

  // first argument
  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(instr_iter, Instruction::kMove);
  move->output()->setPhyRegister(ARGUMENT_REGS[0]);
  move->output()->setDataType(instr->getInput(2)->dataType());
  move->appendInput(instr->releaseInput(2)); // callable

  constexpr PhyLocation TMP_REG = arch::reg_scratch_0_loc;
  int rsp_sub = prepareArgsArray(
      instr_iter,
      num_args,
      flag | PY_VECTORCALL_ARGUMENTS_OFFSET,
      kFirstArg,
      ARGUMENT_REGS[1],
      ARGUMENT_REGS[2]);

  // check if kwnames is provided
  auto last_input = instr->releaseInput(instr->getNumInputs() - 1);
  if (last_input->isImm()) {
    JIT_DCHECK(last_input->getConstant() == 0, "kwnames must be 0 or variable");
    block->allocateInstrBefore(
        instr_iter,
        Instruction::kXor,
        PhyReg(ARGUMENT_REGS[3]),
        PhyReg(ARGUMENT_REGS[3]));
  } else {
    auto move_2 = block->allocateInstrBefore(
        instr_iter, Instruction::kMove, OutPhyReg(ARGUMENT_REGS[3]));
    move_2->appendInput(std::move(last_input));

    // Subtract the length of kwnames (always a tuple) from nargsf (arg2)
    size_t ob_size_offs = offsetof(PyVarObject, ob_size);
    block->allocateInstrBefore(
        instr_iter,
        Instruction::kMove,
        OutPhyReg(TMP_REG),
        Ind(ARGUMENT_REGS[3], (int32_t)ob_size_offs));

    block->allocateInstrBefore(
        instr_iter,
        Instruction::kSub,
        PhyReg(ARGUMENT_REGS[2]),
        PhyReg(TMP_REG));
  }

  return rsp_sub;
}

int rewriteVarArgCall(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  instr->setOpcode(Instruction::kCall);
  auto res = prepareArgsArray(
      instr_iter,
      instr->getNumInputs() - 1, // func is 1st argument
      0,
      1,
      ARGUMENT_REGS[0],
      ARGUMENT_REGS[1]);
  instr->setNumInputs(1);
  return res;
}

// rewrite call instructions:
//   - move function arguments to the right registers.
//   - handle special cases such as JITRT_(Call|Invoke)Function,
//   JITRT_(Call|Get)Method, etc.
RewriteResult rewriteCallInstrs(instr_iter_t instr_iter, Environ* env) {
  auto instr = instr_iter->get();
  if (instr->isVarArgCall()) {
    int rsp_sub = rewriteVarArgCall(instr_iter);
    env->max_arg_buffer_size = std::max<int>(env->max_arg_buffer_size, rsp_sub);
    return kChanged;
  } else if (!instr->isCall() && !instr->isVectorCall()) {
    return kUnchanged;
  }

  auto output = instr->output();
  if (instr->isCall() && instr->getNumInputs() == 1 && output->isNone()) {
    return kUnchanged;
  }

  int rsp_sub = 0;
  auto block = instr->basicblock();

  if (instr->isVectorCall()) {
    rsp_sub = rewriteVectorCallFunctions(instr_iter);
  } else {
    rsp_sub = rewriteRegularFunction(instr_iter);
  }

  instr->setNumInputs(1); // leave function self operand only
  instr->setOpcode(Instruction::kCall);

  auto next_iter = std::next(instr_iter);

  env->max_arg_buffer_size = std::max<int>(env->max_arg_buffer_size, rsp_sub);

  if (output->isNone()) {
    return kChanged;
  }

  const PhyLocation kReturnRegister = output->isFp()
      ? arch::reg_double_return_loc
      : arch::reg_general_return_loc;

  if (!output->isReg() || output->getPhyRegister() != kReturnRegister) {
    if (output->isReg()) {
      block->allocateInstrBefore(
          next_iter,
          Instruction::kMove,
          OutPhyReg(output->getPhyRegister(), output->dataType()),
          PhyReg(kReturnRegister, output->dataType()));
    } else {
      block->allocateInstrBefore(
          next_iter,
          Instruction::kMove,
          OutStk(output->getStackSlot(), output->dataType()),
          PhyReg(kReturnRegister, output->dataType()));
    }
  }
  output->setNone();

  return kChanged;
}

// Replaces ZEXT and SEXT with appropriate MOVE instructions.
RewriteResult rewriteBitExtensionInstrs(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  bool is_sext = instr->opcode() == Instruction::kSext;
  bool is_zext = instr->opcode() == Instruction::kZext;

  if (!is_sext && !is_zext) {
    return kUnchanged;
  }

  auto in = instr->getInput(0);
  auto out = instr->output();
  auto out_size = out->dataType();
  if (in->isImm()) {
    long mask = 0;
    if (out_size == OperandBase::k32bit) {
      mask = 0xffffffffl;
    } else if (out_size == OperandBase::k16bit) {
      mask = 0xffffl;
    } else if (out_size == OperandBase::k8bit) {
      mask = 0xffl;
    } else {
      mask = 0xffffffffffffffffl;
    }
    static_cast<Operand*>(in)->setConstant(in->getConstant() & mask, out_size);
    instr->setOpcode(Instruction::kMove);
    return kChanged;
  }

  auto in_size = in->dataType();
  if (in_size >= out_size) {
    instr->setOpcode(Instruction::kMove);
    return kChanged;
  }

  switch (in_size) {
    case OperandBase::k8bit:
    case OperandBase::k16bit:
      instr->setOpcode(is_sext ? Instruction::kMovSX : Instruction::kMovZX);
      break;
    case OperandBase::k32bit:
      if (is_sext) {
        instr->setOpcode(Instruction::kMovSXD);
      } else {
        // must be unsigned extension from 32 bits to 64 bits.
        // in this case, a 32-bit move will do the work.
        instr->setOpcode(Instruction::kMove);
        instr->output()->setDataType(lir::OperandBase::k32bit);
      }
      break;
    case OperandBase::k64bit:
    case OperandBase::kObject:
      JIT_ABORT("can't be smaller than the maximum size");
    case OperandBase::kDouble:
      JIT_ABORT("A float point number cannot be the input of the instruction.");
  }

  return kChanged;
}

// Add (conditional) branch instructions to the end of each basic blocks when
// necessary.
//
// Currently, conditional branches are always comparing against 0, so they are
// translated directly into machine code, and we don't need to take care of them
// here right now. But once we start to support different conditions (as we
// already did in static compiler), we need to also rewrite conditional branches
// into Jcc instructions.
RewriteResult rewriteBranchInstrs(Function* function) {
  auto& blocks = function->basicblocks();
  bool changed = false;

  for (auto iter = blocks.begin(); iter != blocks.end();) {
    BasicBlock* block = *iter;
    ++iter;

    BasicBlock* next_block = iter == blocks.end() ? nullptr : *iter;

    auto& succs = block->successors();

    if (succs.size() != 1) {
      // skip conditional branches for now.
      continue;
    }

    auto last_instr = block->getLastInstr();
    auto last_opcode =
        last_instr != nullptr ? last_instr->opcode() : Instruction::kNone;
    if (last_opcode == Instruction::kReturn) {
      continue;
    }

    auto successor = succs[0];
    if (successor == next_block && next_block->section() == block->section()) {
      continue;
    }

    if (last_opcode == Instruction::kBranch) {
      continue;
    }

    auto branch = block->allocateInstr(
        Instruction::kBranch,
        last_instr != nullptr ? last_instr->origin() : nullptr);
    branch->allocateLabelInput(succs[0]);

    changed = true;
  }

  return changed ? kChanged : kUnchanged;
}

// rewrite move instructions
// optimize move instruction in the following cases:
//   1. remove the move instruction when source and destination are the same
//   2. rewrite move instruction to xor when the source operand is 0.
RewriteResult optimizeMoveInstrs(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto instr_opcode = instr->opcode();
  if (instr_opcode != Instruction::kMove) {
    return kUnchanged;
  }

  auto out = instr->output();
  auto in = instr->getInput(0);

  // if the input and the output are the same
  if ((out->isReg() || out->isStack()) && in->type() == out->type() &&
      in->getPhyRegOrStackSlot() == out->getPhyRegOrStackSlot()) {
    instr->basicblock()->removeInstr(instr_iter);
    return kRemoved;
  }

  if (in->isImm() && !in->isFp() && in->getConstant() == 0 && out->isReg()) {
    auto in_opnd = dynamic_cast<Operand*>(in);
    JIT_CHECK(
        in_opnd != nullptr,
        "Register allocation should have replaced linked operand {}",
        *in);
    instr->setOpcode(Instruction::kXor);
    auto reg = out->getPhyRegister();
    auto data_type = out->dataType();
    out->setNone();
    instr->setNumInputs(0);
    instr->addOperands(PhyReg{reg, data_type}, PhyReg{reg, data_type});
    return kChanged;
  }

  return kUnchanged;
}

RewriteResult rewriteLoadInstrs(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  if (!instr->isMove() || instr->getNumInputs() != 1 ||
      !instr->getInput(0)->isMem()) {
    return kUnchanged;
  }

  auto out = instr->output();
  JIT_DCHECK(out->isReg(), "Unable to load to a non-register location.");

#if defined(CINDER_X86_64)
  // On x86-64, you can load a 64-bit address directly into a register only if
  // that register is RAX. We check here for that case and return an unchanged
  // instruction if possible.
  if (out->getPhyRegister() == RAX) {
    return kUnchanged;
  }
#endif

  auto in = instr->getInput(0);
  auto mem_addr = reinterpret_cast<intptr_t>(in->getMemoryAddress());

#if defined(CINDER_X86_64)
  // On x86-64, you can load a 32-bit address directly into any register, so
  // check for bounds here and return an unchanged instruction if possible.
  if (fitsSignedInt<32>(mem_addr)) {
    return kUnchanged;
  }
#elif defined(CINDER_AARCH64)
  // aarch64 does not support absolute addressing, so we will always need to
  // rewrite the instruction.
#else
  CINDER_UNSUPPORTED
#endif

  auto block = instr->basicblock();
  block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutPhyReg(out->getPhyRegister()),
      Imm(mem_addr, in->dataType()));

  static_cast<Operand*>(in)->setMemoryIndirect(out->getPhyRegister());

  return kChanged;
}

// Convert CondBranch to Test and BranchCC instructions.
void doRewriteCondBranch(instr_iter_t instr_iter, BasicBlock* next_block) {
  auto instr = instr_iter->get();

  auto input = instr->getInput(0);
  auto block = instr->basicblock();

  // insert test Reg, Reg instruction
  auto size = input->dataType();
  block->allocateInstrBefore(
      instr_iter,
      Instruction::kTest,
      PhyReg(input->getPhyRegister(), size),
      PhyReg(input->getPhyRegister(), size));

  // convert the current CondBranch instruction to a BranchCC instruction
  auto true_block = block->getTrueSuccessor();
  auto false_block = block->getFalseSuccessor();

  BasicBlock* target_block = nullptr;
  BasicBlock* fallthrough_block = nullptr;

  auto opcode = Instruction::kBranchNZ;
  if (true_block == next_block) {
    opcode = Instruction::negateBranchCC(opcode);
    target_block = false_block;
    fallthrough_block = true_block;
  } else {
    target_block = true_block;
    fallthrough_block = false_block;
  }

  instr->setOpcode(opcode);
  instr->setNumInputs(0);

  instr->allocateLabelInput(target_block);

  if (fallthrough_block != next_block ||
      block->section() != next_block->section()) {
    auto fallthrough_branch =
        block->allocateInstr(Instruction::kBranch, instr->origin());
    fallthrough_branch->allocateLabelInput(fallthrough_block);
  }
}

// Negate BranchCC instructions based on the next (fallthrough) basic block.
void doRewriteBranchCC(instr_iter_t instr_iter, BasicBlock* next_block) {
  auto instr = instr_iter->get();
  auto block = instr->basicblock();

  auto true_bb = block->getTrueSuccessor();
  auto false_bb = block->getFalseSuccessor();
  BasicBlock* fallthrough_bb = nullptr;

  if (true_bb == next_block) {
    instr->setOpcode(Instruction::negateBranchCC(instr->opcode()));
    instr->allocateLabelInput(false_bb);
    fallthrough_bb = true_bb;
  } else {
    instr->allocateLabelInput(true_bb);
    fallthrough_bb = false_bb;
  }

  if (fallthrough_bb != next_block ||
      block->section() != next_block->section()) {
    auto fallthrough_branch =
        block->allocateInstr(Instruction::kBranch, instr->origin());
    fallthrough_branch->allocateLabelInput(fallthrough_bb);
  }
}

// Convert CondBranch and BranchCC instructions.
RewriteResult rewriteCondBranch(Function* function) {
  auto& blocks = function->basicblocks();

  bool changed = false;
  for (auto iter = blocks.begin(); iter != blocks.end();) {
    BasicBlock* block = *iter;
    ++iter;

    auto instr_iter = block->getLastInstrIter();
    if (instr_iter == block->instructions().end()) {
      continue;
    }

    BasicBlock* next_block = (iter != blocks.end() ? *iter : nullptr);

    auto instr = instr_iter->get();

    if (instr->isCondBranch()) {
      doRewriteCondBranch(instr_iter, next_block);
      changed = true;
    } else if (instr->isBranchCC() && instr->getNumInputs() == 0) {
      doRewriteBranchCC(instr_iter, next_block);
      changed = true;
    }
  }

  return changed ? kChanged : kUnchanged;
}

RewriteResult rewriteBinaryOpInstrs(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  // For a binary operation:
  //
  //   OutReg = BinOp Reg0, Reg1
  //
  // find if OutReg == Reg0 or OutReg == Reg1, so we can rewrite to the
  // two-operand form and save a move in autogen.cpp.
  //
  // Performing this rewrite also makes it safe to not set inputs_live_across
  // on binary ops that write their output before reading all of their inputs:
  // if the output is the same register as one of the inputs, it will be
  // rewritten into the two-operand form here.
  //
  // Subtraction is anticommutative, so we could in theory support it here by
  // negating the output in the (OutReg == Reg1) case. But the Move we're
  // trying to avoid is probably going to be cheaper than the negation anyway,
  // so skip that case. And since we're skipping that case, we have to set
  // inputs_live_across for Sub and Fsub, meaning they can be left out of this
  // rewrite entirely.

  if (!instr->isAdd() && !instr->isXor() && !instr->isAnd() && !instr->isOr() &&
      !instr->isMul() && !instr->isFadd() && !instr->isFmul()) {
    return kUnchanged;
  }

  if (!instr->output()->isReg() || !instr->getInput(0)->isReg()) {
    return kUnchanged;
  }

  auto out_reg = instr->output()->getPhyRegister();
  auto in0_reg = instr->getInput(0)->getPhyRegister();

  if (out_reg == in0_reg) {
    // Remove the output. The code generator will use the first input as the
    // output (and also the first input).
    instr->output()->setNone();
    return kChanged;
  }

  auto in1 = instr->getInput(1);
  auto in1_reg =
      in1->isReg() ? in1->getPhyRegister() : PhyLocation::REG_INVALID;
  if (out_reg == in1_reg) {
    instr->output()->setNone();

    auto opnd0 = instr->removeInput(0);
    instr->appendInput(std::move(opnd0));
    return kChanged;
  }

  return kUnchanged;
}

#if defined(CINDER_X86_64)
// Rewrite 8-bit multiply to use single-operand imul.
RewriteResult rewriteByteMultiply(instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();

  if (!instr->isMul() || instr->getNumInputs() < 2) {
    return kUnchanged;
  }

  Operand* input0 = static_cast<Operand*>(instr->getInput(0));

  if (input0->dataType() > OperandBase::k8bit) {
    return kUnchanged;
  }

  Operand* output = static_cast<Operand*>(instr->output());
  PhyLocation in_reg = input0->getPhyRegister();
  PhyLocation out_reg = in_reg;

  if (output->isReg()) {
    out_reg = output->getPhyRegister();
  }

  BasicBlock* block = instr->basicblock();
  if (in_reg != AL) {
    block->allocateInstrBefore(
        instr_iter,
        Instruction::kMove,
        OutPhyReg(AL, OperandBase::k8bit),
        PhyReg(in_reg, OperandBase::k8bit));
    input0->setPhyRegister(AL);
  }
  // asmjit only recognizes 8-bit imul if RAX is passed as 16-bit.
  input0->setDataType(OperandBase::k16bit);
  output->setNone(); // no output means first input is also output
  if (out_reg != RAX) {
    block->allocateInstrBefore(
        std::next(instr_iter),
        Instruction::kMove,
        OutPhyReg(out_reg, OperandBase::k8bit),
        PhyReg(AL, OperandBase::k8bit));
  }
  return kChanged;
}
#endif

bool insertMoveToRegister(
    BasicBlock* block,
    instr_iter_t instr_iter,
    Operand* op,
    PhyLocation location) {
  // Already in the right place.
  if (op->isReg() && op->getPhyRegister() == location) {
    return false;
  }

  auto data_type = op->dataType();
  auto move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutPhyReg(location, data_type));
  if (op->isReg()) {
    move->addOperands(PhyReg(op->getPhyRegister(), data_type));
  } else if (op->isImm()) {
    move->addOperands(Imm(op->getConstant(), data_type));
  } else if (op->isStack()) {
    move->addOperands(Stk(op->getStackSlot(), data_type));
  } else if (op->isMem()) {
    JIT_ABORT("Unsupported: div from mem");
  } else {
    JIT_ABORT("Unexpected operand base: {}", static_cast<int>(op->type()));
  }

  op->setPhyRegister(location);
  op->setDataType(data_type);
  return true;
}

#if defined(CINDER_X86_64)
// Rewrite division instructions to use correct registers.
RewriteResult rewriteDivide(instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  if (!instr->isDiv() && !instr->isDivUn()) {
    return kUnchanged;
  }

  bool changed = false;
  Operand* output = static_cast<Operand*>(instr->output());

  BasicBlock* block = instr->basicblock();

  Operand* dividend_upper = nullptr;
  Operand* dividend_lower;
  if (instr->getNumInputs() == 3) {
    dividend_upper = static_cast<Operand*>(instr->getInput(0));
    dividend_lower = static_cast<Operand*>(instr->getInput(1));
  } else {
    dividend_lower = static_cast<Operand*>(instr->getInput(0));
  }

  PhyLocation out_reg = RAX;
  if (output->type() != OperandBase::kNone) {
    out_reg = output->getPhyRegister();
  } else {
    JIT_CHECK(dividend_lower->isReg(), "input should be in register");
    out_reg = dividend_lower->getPhyRegister();
  }

  if (dividend_lower->dataType() == OperandBase::k8bit) {
    // 8-bit division uses 16-bits from ax instead of using
    // dx as the upper word, so we need to sign extend it to
    // be a 16-bit input (we'll use the size from the divisor
    // as the size of the instruction, setting the size on
    // divided_lower here is just tracking that we've done
    // the transformation).  When we do this we'll re-write
    // it down to the 2 input form and make dividend_lower
    // be 16-bit.
    JIT_CHECK(
        instr->getNumInputs() == 3,
        "8-bit should always start with 3 operands");
    auto move = block->allocateInstrBefore(
        instr_iter,
        dividend_lower->isImm() ? Instruction::kMove
            : instr->isDiv()    ? Instruction::kMovSX
                                : Instruction::kMovZX,
        OutPhyReg(AX, OperandBase::k16bit));

    if (dividend_lower->isImm()) {
      dividend_lower->setDataType(OperandBase::k16bit);
    }

    auto divisor_removed = instr->removeInput(2);
    auto div_lower_removed = instr->removeInput(1);
    move->appendInput(std::move(div_lower_removed));

    instr->removeInput(0); // Imm/rdx, no longer used

    instr->addOperands(PhyReg(AX, OperandBase::k16bit));
    instr->appendInput(std::move(divisor_removed));
    changed = true;
  } else {
    // dividend lower needs to be in rax, we reserved the register
    // in reg_alloc.
    changed |= insertMoveToRegister(block, instr_iter, dividend_lower, RAX);

    if (dividend_upper != nullptr &&
        (!dividend_upper->isReg() ||
         dividend_upper->getPhyRegister() != PhyLocation::RDX)) {
      JIT_CHECK(
          (dividend_upper->isImm() && dividend_upper->getConstant() == 0),
          "only immediate 0 is supported");

      if (instr->isDiv()) {
        // extend rax into rdx
        Instruction::Opcode extend;
        switch (dividend_lower->sizeInBits()) {
          case 16:
            extend = Instruction::kCwd;
            break;
          case 32:
            extend = Instruction::kCdq;
            break;
          case 64:
            extend = Instruction::kCqo;
            break;
          default:
            Py_UNREACHABLE();
        }
        block->allocateInstrBefore(
            instr_iter, extend, OutPhyReg(RDX), PhyReg(RAX));
      } else {
        // zero rdx
        block->allocateInstrBefore(
            instr_iter, Instruction::kXor, PhyReg(RDX), PhyReg(RDX));
      }

      dividend_upper->setPhyRegister(PhyLocation::RDX);
      dividend_upper->setDataType(dividend_lower->dataType());
      changed = true;
    }
  }

  if (out_reg != RAX) {
    block->allocateInstrBefore(
        std::next(instr_iter),
        Instruction::kMove,
        OutPhyReg(out_reg, dividend_lower->dataType()),
        PhyReg(PhyLocation::RAX, dividend_lower->dataType()));
    changed = true;
  }
  output->setNone();

  return changed ? kChanged : kUnchanged;
}
#endif

// record register-to-memory moves and map between them.
class RegisterToMemoryMoves {
 public:
  void addRegisterToMemoryMove(
      PhyLocation from,
      PhyLocation to,
      instr_iter_t instr_iter) {
    JIT_DCHECK(
        from.is_register() && to.is_memory(),
        "Must be a move from register to memory");
    invalidateMemory(to);
    invalidateRegister(from);

    reg_to_mem_[from] = to;
    mem_to_reg_[to] = {from, instr_iter};
  }

  void invalidate(PhyLocation loc) {
    if (loc.is_register()) {
      invalidateRegister(loc);
    } else {
      invalidateMemory(loc);
    }
  }

  PhyLocation getRegisterFromMemory(PhyLocation mem) {
    auto iter = mem_to_reg_.find(mem);
    if (iter != mem_to_reg_.end()) {
      return iter->second.first;
    }

    return PhyLocation::REG_INVALID;
  }

  std::optional<instr_iter_t> getInstrFromMemory(PhyLocation mem) {
    auto iter = mem_to_reg_.find(mem);
    if (iter == mem_to_reg_.end()) {
      return std::nullopt;
    }

    return iter->second.second;
  }

  void clear() {
    reg_to_mem_.clear();
    mem_to_reg_.clear();
  }

 private:
  UnorderedMap<PhyLocation, PhyLocation> reg_to_mem_;
  UnorderedMap<PhyLocation, std::pair<PhyLocation, instr_iter_t>> mem_to_reg_;

  void invalidateRegister(PhyLocation reg) {
    auto iter = reg_to_mem_.find(reg);
    if (iter != reg_to_mem_.end()) {
      mem_to_reg_.erase(iter->second);
      reg_to_mem_.erase(iter);
    }
  }
  void invalidateMemory(PhyLocation mem) {
    auto iter = mem_to_reg_.find(mem);
    if (iter != mem_to_reg_.end()) {
      reg_to_mem_.erase(iter->second.first);
      mem_to_reg_.erase(iter);
    }
  }
};

// Replace memory input with register when possible within a basic block and
// remove the unnecessary moves after the replacement.
RewriteResult optimizeMoveSequence(BasicBlock* basicblock) {
  auto changed = kUnchanged;
  RegisterToMemoryMoves registerMemoryMoves;

  for (auto instr_iter = basicblock->instructions().begin();
       instr_iter != basicblock->instructions().end();
       ++instr_iter) {
    auto& instr = *instr_iter;
    // TODO: do not optimize for yield for now. They need to be special cased.
    if (!instr->isAnyYield()) {
      auto out_reg = instr->output()->isReg()
          ? instr->output()->getPhyRegister()
          : PhyLocation::REG_INVALID;
      // for moves only we can generate A = Move A, which will get optimized out
      if (instr->isMove()) {
        out_reg = PhyLocation::REG_INVALID;
      }
      instr->foreachInputOperand([&](OperandBase* operand) {
        if (!operand->isStack()) {
          return;
        }

        PhyLocation stack_slot = operand->getStackSlot();
        auto reg = registerMemoryMoves.getRegisterFromMemory(stack_slot);
        if (reg == PhyLocation::REG_INVALID || reg == out_reg) {
          return;
        }

        auto opnd = static_cast<Operand*>(operand);
        auto data_type = opnd->dataType();
        auto old_opnd = fmt::to_string(*opnd);
        opnd->setPhyRegister(reg);
        JIT_CHECK(
            bitSize(data_type) == bitSize(opnd->dataType()),
            "Incorrectly changed data type from {} to {} in "
            "{}",
            old_opnd,
            *opnd,
            *instr);
        changed = kChanged;

        // if the stack location operand can be replaced by the register it came
        // from and this is the last use of the operand, we can remove the move
        // instruction moving from the register to the stack location.
        if (opnd->isLastUse()) {
          auto opt_iter = registerMemoryMoves.getInstrFromMemory(stack_slot);
          JIT_CHECK(opt_iter.has_value(), "There must be a def instruction.");
          basicblock->instructions().erase(*opt_iter);
        }
      });
    }

    auto invalidateOperand = [&](const OperandBase* opnd) {
      if (opnd->isStack() || opnd->isReg()) {
        registerMemoryMoves.invalidate(opnd->getPhyRegOrStackSlot());
      }
    };

    if (instr->isMove() || instr->isPush() || instr->isPop()) {
      if (instr->isMove()) {
        Operand* out = instr->output();
        OperandBase* in = instr->getInput(0);
        if (out->isStack() && in->isReg()) {
          registerMemoryMoves.addRegisterToMemoryMove(
              in->getPhyRegister(), out->getStackSlot(), instr_iter);
        } else {
          invalidateOperand(out);
        }
      } else if (instr->isPop()) {
        auto opnd = instr->output();
        invalidateOperand(opnd);
      }
    } else {
      // TODO: for now, we always clear the cache when we hit an instruction
      // other than MOVE, PUSH, and POP, since our main goal is to optimize the
      // operand copies before a function call. Consider a more fine-grained
      // control of what to invalidate for better results.
      registerMemoryMoves.clear();
    }
  }
  return changed;
}

} // namespace

void PostRegAllocRewrite::registerRewrites() {
  registerOneRewriteFunction(rewriteCallInstrs);
  registerOneRewriteFunction(rewriteBitExtensionInstrs);
  registerOneRewriteFunction(rewriteBranchInstrs);
  registerOneRewriteFunction(rewriteLoadInstrs);
  registerOneRewriteFunction(rewriteCondBranch);
  registerOneRewriteFunction(rewriteBinaryOpInstrs);
  registerOneRewriteFunction(removePhiInstructions);

#if defined(CINDER_X86_64)
  registerOneRewriteFunction(rewriteByteMultiply);
#endif

  registerOneRewriteFunction(optimizeMoveSequence, 1);
  registerOneRewriteFunction(optimizeMoveInstrs, 1);

#if defined(CINDER_X86_64)
  registerOneRewriteFunction(rewriteDivide);
#endif
}

} // namespace jit::lir
