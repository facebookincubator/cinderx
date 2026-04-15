// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/autogen.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/gen_asm_utils.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/module_state.h"

using namespace asmjit;
using namespace jit::lir;
using namespace jit::codegen;

namespace jit::codegen::autogen {

void translateLeaLabel(Environ* env, const Instruction* instr);
void TranslateGuard(Environ* env, const Instruction* instr);
void TranslateDeoptPatchpoint(Environ* env, const Instruction* instr);
void TranslateCompare(Environ* env, const Instruction* instr);
void translateLoadThreadState(Environ* env, const Instruction* instr);
void translateYieldInitial(Environ* env, const Instruction* instr);
void translateYieldValue(Environ* env, const Instruction* instr);
void translateStoreGenYieldPoint(Environ* env, const Instruction* instr);
void translateStoreGenYieldFromPoint(Environ* env, const Instruction* instr);
void translateBranchToYieldExit(Environ* env, const Instruction*);
void translateResumeGenYield(Environ* env, const Instruction* instr);
void translateYieldExitPoint(Environ* env, const Instruction*);
void translateEpilogueEnd(Environ* env, const Instruction* instr);
void translateIntToBool(Environ* env, const Instruction* instr);
void translatePrologue(Environ* env, const Instruction*);
void translateSetupFrame(Environ* env, const Instruction*);
void translateIndirectJump(Environ* env, const Instruction* instr);

arch::Mem AsmIndirectOperandBuilder(const OperandBase* operand) {
  JIT_DCHECK(operand->isInd(), "operand should be an indirect reference");

#if defined(CINDER_X86_64)
  auto indirect = operand->getMemoryIndirect();

  OperandBase* base = indirect->getBaseRegOperand();
  OperandBase* index = indirect->getIndexRegOperand();

  if (index == nullptr) {
    return asmjit::x86::ptr(
        x86::gpq(base->getPhyRegister().loc), indirect->getOffset());
  } else {
    return asmjit::x86::ptr(
        x86::gpq(base->getPhyRegister().loc),
        x86::gpq(index->getPhyRegister().loc),
        indirect->getMultipiler(),
        indirect->getOffset());
  }
#elif defined(CINDER_AARCH64)
  JIT_ABORT("Unreachable.");
#else
  CINDER_UNSUPPORTED
  return arch::Mem();
#endif
}

// Resolves the operand size in bits, respecting the instruction's
// OperandSizeType property.
int getOperandSize(const Instruction* instr, const OperandBase* operand) {
  auto size_type = InstrProperty::getProperties(instr).opnd_size_type;
  switch (size_type) {
    case kAlways64:
      return 64;
    case kOut: {
      // Match LIROperandMapper<0> behavior: use the output if present,
      // otherwise use input 0 (which post-alloc rewrites may have resized).
      if (instr->getNumOutputs() > 0) {
        return static_cast<int>(instr->output()->sizeInBits());
      }
      return static_cast<int>(instr->getInput(0)->sizeInBits());
    }
    case kDefault:
    default:
      return static_cast<int>(operand->sizeInBits());
  }
}

// Returns the appropriately-sized Gp register for a given operand, respecting
// the instruction's OperandSizeType property.
arch::Gp getReg(const Instruction* instr, const OperandBase* operand) {
  JIT_CHECK(operand->isReg(), "Expected a register for getReg");
  int size = getOperandSize(instr, operand);
  auto reg = operand->getPhyRegister().loc;
#if defined(CINDER_X86_64)
  switch (size) {
    case 8:
      return asmjit::x86::gpb(reg);
    case 16:
      return asmjit::x86::gpw(reg);
    case 32:
      return asmjit::x86::gpd(reg);
    case 64:
      return asmjit::x86::gpq(reg);
  }
#elif defined(CINDER_AARCH64)
  switch (size) {
    case 8:
    case 16:
      JIT_ABORT("Currently unsupported size.");
    case 32:
      return asmjit::a64::w(reg);
    case 64:
      return asmjit::a64::x(reg);
  }
#else
  CINDER_UNSUPPORTED
#endif
  JIT_ABORT("Unexpected operand size {}", size);
}

// Returns an arch::Mem for a given memory operand (stack, mem, or indirect),
// with size set according to the instruction's OperandSizeType property.
arch::Mem getMem(const Instruction* instr, const OperandBase* operand) {
#if defined(CINDER_X86_64)
  int size = getOperandSize(instr, operand) / 8;
  asmjit::x86::Mem memptr;
  if (operand->isStack()) {
    memptr = asmjit::x86::ptr(asmjit::x86::rbp, operand->getStackSlot().loc);
  } else if (operand->isMem()) {
    memptr = asmjit::x86::ptr(
        reinterpret_cast<uint64_t>(operand->getMemoryAddress()));
  } else if (operand->isInd()) {
    memptr = AsmIndirectOperandBuilder(operand);
  } else {
    JIT_ABORT("Unsupported operand type for getMem.");
  }
  memptr.setSize(size);
  return memptr;
#elif defined(CINDER_AARCH64)
  if (!operand->isStack()) {
    JIT_ABORT("Unreachable.");
  }
  int32_t loc = operand->getStackSlot().loc;
  JIT_CHECK(loc >= -256 && loc < 256, "Stack slot out of range");
  return arch::ptr_offset(arch::fp, loc);
#else
  CINDER_UNSUPPORTED
  return arch::Mem();
#endif
}

asmjit::Imm getImm(const OperandBase* operand) {
  return asmjit::Imm(operand->getConstant());
}

asmjit::Label getLabel(Environ* env, const OperandBase* operand) {
  if (operand->getDefine()->hasAsmLabel()) {
    return operand->getDefine()->getAsmLabel();
  }
  return map_get(env->block_label_map, operand->getBasicBlock());
}

#if defined(CINDER_AARCH64)
namespace {
void translateLea(Environ* env, const Instruction* instr);
void translateCall(Environ* env, const Instruction* instr);
void translateMove(Environ* env, const Instruction* instr);
void translateMovConstPool(Environ* env, const Instruction* instr);
void translateMovZX(Environ* env, const Instruction* instr);
void translateMovSX(Environ* env, const Instruction* instr);
void translateMovSXD(Environ* env, const Instruction* instr);
void translateUnreachable(Environ* env, const Instruction* instr);
void translateNegate(Environ* env, const Instruction* instr);
void translateInvert(Environ* env, const Instruction* instr);
void translateAdd(Environ* env, const Instruction* instr);
void translateSub(Environ* env, const Instruction* instr);
void translateAnd(Environ* env, const Instruction* instr);
void translateOr(Environ* env, const Instruction* instr);
void translateXor(Environ* env, const Instruction* instr);
void translateMul(Environ* env, const Instruction* instr);
void translateMulAdd(Environ* env, const Instruction* instr);
void translateDiv(Environ* env, const Instruction* instr);
void translateDivUn(Environ* env, const Instruction* instr);
void translatePush(Environ* env, const Instruction* instr);
void translatePop(Environ* env, const Instruction* instr);
void translateExchange(Environ* env, const Instruction* instr);
void translateCmp(Environ* env, const Instruction* instr);
void translateTst(Environ* env, const Instruction* instr);
void translateBitTest(Environ* env, const Instruction* instr);
void translateInc(Environ* env, const Instruction* instr);
void translateDec(Environ* env, const Instruction* instr);
void translateSelect(Environ* env, const Instruction* instr);
} // namespace
#endif

// Translates a single LIR instruction to machine code.
void AutoTranslator::translateInstr(Environ* env, const Instruction* instr)
    const {
  auto opcode = instr->opcode();
  switch (opcode) {
    case Instruction::kBind:
      return;
#if defined(CINDER_X86_64)
    case Instruction::kLea: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isLabel()) {
        translateLeaLabel(env, instr);
      } else {
        env->as->lea(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Instruction::kMoveRelaxed: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (output->isReg()) {
        env->as->mov(getReg(instr, output), getMem(instr, input));
      } else if (input->isReg()) {
        env->as->mov(getMem(instr, output), getReg(instr, input));
      } else {
        env->as->mov(getMem(instr, output), getImm(input));
      }
      return;
    }
    case Instruction::kMovZX: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->movzx(getReg(instr, output), getReg(instr, input));
      } else {
        env->as->movzx(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Instruction::kMovSX: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->movsx(getReg(instr, output), getReg(instr, input));
      } else {
        env->as->movsx(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Instruction::kMovSXD: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->movsxd(getReg(instr, output), getReg(instr, input));
      } else {
        env->as->movsxd(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Instruction::kUnreachable:
      env->as->ud2();
      return;
    case Instruction::kDiv: {
      auto numInputs = instr->getNumInputs();
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (numInputs == 3) {
        auto* in2 = instr->getInput(2);

        if (in2->isReg()) {
          env->as->idiv(
              getReg(instr, in0), getReg(instr, in1), getReg(instr, in2));
        } else {
          env->as->idiv(
              getReg(instr, in0), getReg(instr, in1), getMem(instr, in2));
        }
      } else {
        if (in1->isReg()) {
          env->as->idiv(getReg(instr, in0), getReg(instr, in1));
        } else {
          env->as->idiv(getReg(instr, in0), getMem(instr, in1));
        }
      }
      return;
    }
    case Instruction::kDivUn: {
      auto numInputs = instr->getNumInputs();
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (numInputs == 3) {
        auto* in2 = instr->getInput(2);

        if (in2->isReg()) {
          env->as->div(
              getReg(instr, in0), getReg(instr, in1), getReg(instr, in2));
        } else {
          env->as->div(
              getReg(instr, in0), getReg(instr, in1), getMem(instr, in2));
        }
      } else {
        if (in1->isReg()) {
          env->as->div(getReg(instr, in0), getReg(instr, in1));
        } else {
          env->as->div(getReg(instr, in0), getMem(instr, in1));
        }
      }
      return;
    }
    case Instruction::kPush: {
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->push(getReg(instr, input));
      } else if (input->isImm()) {
        env->as->push(getImm(input));
      } else {
        env->as->push(getMem(instr, input));
      }
      return;
    }
    case Instruction::kPop: {
      auto* output = instr->output();

      if (output->isReg()) {
        env->as->pop(getReg(instr, output));
      } else {
        env->as->pop(getMem(instr, output));
      }
      return;
    }
    case Instruction::kCdq: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cdq(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Instruction::kCwd: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cwd(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Instruction::kCqo: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cqo(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Instruction::kTest: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->test(getReg(instr, in0), getReg(instr, in1));
      return;
    }
    case Instruction::kBranch:
      env->as->jmp(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchZ:
      env->as->jz(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNZ:
      env->as->jnz(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchA:
      env->as->ja(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchB:
      env->as->jb(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchAE:
      env->as->jae(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchBE:
      env->as->jbe(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchG:
      env->as->jg(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchL:
      env->as->jl(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchGE:
      env->as->jge(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchLE:
      env->as->jle(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchC:
      env->as->jc(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNC:
      env->as->jnc(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchO:
      env->as->jo(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNO:
      env->as->jno(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchS:
      env->as->js(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNS:
      env->as->jns(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchE:
      env->as->je(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNE:
      env->as->jne(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kGuard:
      TranslateGuard(env, instr);
      return;
    case Instruction::kDeoptPatchpoint:
      TranslateDeoptPatchpoint(env, instr);
      return;
    case Instruction::kLoadThreadState:
      translateLoadThreadState(env, instr);
      return;
    case Instruction::kYieldInitial:
      translateYieldInitial(env, instr);
      return;
    case Instruction::kYieldValue:
      translateYieldValue(env, instr);
      return;
    case Instruction::kStoreGenYieldPoint:
      translateStoreGenYieldPoint(env, instr);
      return;
    case Instruction::kStoreGenYieldFromPoint:
      translateStoreGenYieldFromPoint(env, instr);
      return;
    case Instruction::kBranchToYieldExit:
      translateBranchToYieldExit(env, instr);
      return;
    case Instruction::kResumeGenYield:
      translateResumeGenYield(env, instr);
      return;
    case Instruction::kYieldExitPoint:
      translateYieldExitPoint(env, instr);
      return;
    case Instruction::kEpilogueEnd:
      translateEpilogueEnd(env, instr);
      return;
    case Instruction::kIntToBool:
      translateIntToBool(env, instr);
      return;
    case Instruction::kPrologue:
      translatePrologue(env, instr);
      return;
    case Instruction::kSetupFrame:
      translateSetupFrame(env, instr);
      return;
    case Instruction::kIndirectJump:
      translateIndirectJump(env, instr);
      return;
    case Instruction::kInc: {
      auto* input = instr->getInput(0);

      if (input->isStack()) {
        env->as->inc(getMem(instr, input));
      } else {
        env->as->inc(getReg(instr, input));
      }
      return;
    }
    case Instruction::kDec: {
      auto* input = instr->getInput(0);

      if (input->isStack()) {
        env->as->dec(getMem(instr, input));
      } else {
        env->as->dec(getReg(instr, input));
      }
      return;
    }
    case Instruction::kBitTest: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->bt(getReg(instr, in0), getImm(in1));
      return;
    }
    case Instruction::kSelect: {
      auto output = getReg(instr, instr->output());
      auto condition = getReg(instr, instr->getInput(0));

      env->as->mov(output, getImm(instr->getInput(2)));
      env->as->test(condition, condition);
      env->as->cmovnz(output, getReg(instr, instr->getInput(1)));
      return;
    }
    case Instruction::kEqual:
    case Instruction::kNotEqual:
    case Instruction::kGreaterThanUnsigned:
    case Instruction::kGreaterThanEqualUnsigned:
    case Instruction::kLessThanUnsigned:
    case Instruction::kLessThanEqualUnsigned:
    case Instruction::kGreaterThanSigned:
    case Instruction::kGreaterThanEqualSigned:
    case Instruction::kLessThanSigned:
    case Instruction::kLessThanEqualSigned:
      TranslateCompare(env, instr);
      return;
    case Instruction::kFadd: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->addsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->addsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Instruction::kFsub: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->subsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->subsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Instruction::kFmul: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->mulsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->mulsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Instruction::kFdiv: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->divsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->divsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Instruction::kExchange: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (output->isVecD()) {
        auto left = getVecD(output);
        auto right = getVecD(input);

        env->as->pxor(left, right);
        env->as->pxor(right, left);
        env->as->pxor(left, right);
      } else {
        env->as->xchg(getReg(instr, output), getReg(instr, input));
      }
      return;
    }
    case Instruction::kCmp: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (in0->isVecD()) {
        env->as->comisd(getVecD(in0), getVecD(in1));
      } else if (in1->isImm()) {
        env->as->cmp(getReg(instr, in0), getImm(in1));
      } else {
        env->as->cmp(getReg(instr, in0), getReg(instr, in1));
      }
      return;
    }
    case Instruction::kNegate: {
      if (instr->getNumOutputs() == 0) {
        env->as->neg(getReg(instr, instr->getInput(0)));
      } else {
        auto* output = instr->output();
        auto* input = instr->getInput(0);

        if (input->isImm()) {
          env->as->mov(
              getReg(instr, output), asmjit::Imm(-input->getConstant()));
        } else {
          if (input->isStack()) {
            env->as->mov(getReg(instr, output), getMem(instr, input));
          } else {
            env->as->mov(getReg(instr, output), getReg(instr, input));
          }
          env->as->neg(getReg(instr, output));
        }
      }
      return;
    }
    case Instruction::kInvert: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isImm()) {
        env->as->mov(getReg(instr, output), asmjit::Imm(~input->getConstant()));
      } else {
        if (input->isStack()) {
          env->as->mov(getReg(instr, output), getMem(instr, input));
        } else {
          env->as->mov(getReg(instr, output), getReg(instr, input));
        }
        env->as->not_(getReg(instr, output));
      }
      return;
    }
    case Instruction::kAdd:
    case Instruction::kSub:
    case Instruction::kAnd:
    case Instruction::kOr:
    case Instruction::kXor:
    case Instruction::kMul: {
      auto emitOp = [&](const auto& dst, const auto& src) {
        // NOLINTNEXTLINE(clang-diagnostic-switch-enum)
        switch (opcode) {
          case Instruction::kAdd:
            env->as->add(dst, src);
            break;
          case Instruction::kSub:
            env->as->sub(dst, src);
            break;
          case Instruction::kAnd:
            env->as->and_(dst, src);
            break;
          case Instruction::kOr:
            env->as->or_(dst, src);
            break;
          case Instruction::kXor:
            env->as->xor_(dst, src);
            break;
          case Instruction::kMul:
            env->as->imul(dst, src);
            break;
          default:
            JIT_ABORT("unexpected opcode");
        }
      };

      if (instr->getNumOutputs() > 0) {
        auto* output = instr->output();
        auto* in0 = instr->getInput(0);
        auto* in1 = instr->getInput(1);

        env->as->mov(getReg(instr, output), getReg(instr, in0));
        if (in1->isImm()) {
          emitOp(getReg(instr, output), getImm(in1));
        } else if (in1->isStack()) {
          emitOp(getReg(instr, output), getMem(instr, in1));
        } else {
          emitOp(getReg(instr, output), getReg(instr, in1));
        }
      } else {
        auto* in0 = instr->getInput(0);
        auto* in1 = instr->getInput(1);

        if (in1->isImm()) {
          emitOp(getReg(instr, in0), getImm(in1));
        } else if (in1->isStack()) {
          emitOp(getReg(instr, in0), getMem(instr, in1));
        } else {
          emitOp(getReg(instr, in0), getReg(instr, in1));
        }
      }
      return;
    }
    case Instruction::kTest32: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->test(
          asmjit::x86::gpd(in0->getPhyRegister().loc),
          asmjit::x86::gpd(in1->getPhyRegister().loc));
      return;
    }
    case Instruction::kInt64ToDouble: {
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->cvtsi2sd(getVecD(instr->output()), getReg(instr, input));
      } else {
        env->as->cvtsi2sd(getVecD(instr->output()), getMem(instr, input));
      }
      return;
    }
    case Instruction::kCall: {
      auto* input = instr->getInput(0);

      if (input->isImm()) {
        env->as->call(getImm(input));
      } else if (input->isStack()) {
        env->as->call(getMem(instr, input));
      } else {
        env->as->call(getReg(instr, input));
      }

      asmjit::Label label = env->as->newLabel();
      env->as->bind(label);
      if (instr->origin()) {
        env->pending_debug_locs.emplace_back(label, instr->origin());
      }
      return;
    }
    case Instruction::kMove: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (output->isReg() && output->isVecD()) {
        if (input->isReg() && input->isVecD()) {
          env->as->movsd(getVecD(output), getVecD(input));
        } else if (input->isReg()) {
          env->as->movq(getVecD(output), getReg(instr, input));
        } else {
          env->as->movsd(getVecD(output), getMem(instr, input));
        }
      } else if (output->isReg()) {
        if (input->isReg() && input->isVecD()) {
          env->as->movq(getReg(instr, output), getVecD(input));
        } else if (input->isReg()) {
          env->as->mov(getReg(instr, output), getReg(instr, input));
        } else if (input->isImm()) {
          env->as->mov(getReg(instr, output), getImm(input));
        } else {
          env->as->mov(getReg(instr, output), getMem(instr, input));
        }
      } else {
        if (input->isReg() && input->isVecD()) {
          env->as->movsd(getMem(instr, output), getVecD(input));
        } else if (input->isReg()) {
          env->as->mov(getMem(instr, output), getReg(instr, input));
        } else {
          env->as->mov(getMem(instr, output), getImm(input));
        }
      }
      return;
    }
    case Instruction::kNone:
    case Instruction::kNop:
    case Instruction::kVectorCall:
    case Instruction::kVarArgCall:
    case Instruction::kSext:
    case Instruction::kZext:
    case Instruction::kMulAdd:
    case Instruction::kLShift:
    case Instruction::kRShift:
    case Instruction::kRShiftUn:
    case Instruction::kLoadArg:
    case Instruction::kLoadSecondCallResult:
    case Instruction::kMovConstPool:
    case Instruction::kCondBranch:
    case Instruction::kPhi:
    case Instruction::kReturn:
      JIT_ABORT("Unexpected opcode {} in translateInstr", (int)opcode);
#elif defined(CINDER_AARCH64)
    case Instruction::kLea: {
      auto* input = instr->getInput(0);

      if (input->isLabel()) {
        translateLeaLabel(env, instr);
      } else {
        translateLea(env, instr);
      }
      return;
    }
    case Instruction::kMoveRelaxed:
      translateMove(env, instr);
      return;
    case Instruction::kMovZX:
      translateMovZX(env, instr);
      return;
    case Instruction::kMovSX:
      translateMovSX(env, instr);
      return;
    case Instruction::kMovSXD:
      translateMovSXD(env, instr);
      return;
    case Instruction::kUnreachable:
      translateUnreachable(env, instr);
      return;
    case Instruction::kDiv:
      translateDiv(env, instr);
      return;
    case Instruction::kDivUn:
      translateDivUn(env, instr);
      return;
    case Instruction::kPush:
      translatePush(env, instr);
      return;
    case Instruction::kPop:
      translatePop(env, instr);
      return;
    case Instruction::kTest:
      translateTst(env, instr);
      return;
    case Instruction::kBranch:
      env->as->b(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchZ:
      env->as->b_eq(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNZ:
      env->as->b_ne(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchA:
      env->as->b_hi(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchB:
      env->as->b_lo(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchAE:
      env->as->b_hs(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchBE:
      env->as->b_ls(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchG:
      env->as->b_gt(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchL:
      env->as->b_lt(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchGE:
      env->as->b_ge(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchLE:
      env->as->b_le(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchC:
      env->as->b_cs(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNC:
      env->as->b_cc(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchO:
      env->as->b_vs(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNO:
      env->as->b_vc(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchS:
      env->as->b_mi(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNS:
      env->as->b_pl(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchE:
      env->as->b_eq(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kBranchNE:
      env->as->b_ne(getLabel(env, instr->getInput(0)));
      return;
    case Instruction::kGuard:
      TranslateGuard(env, instr);
      return;
    case Instruction::kDeoptPatchpoint:
      TranslateDeoptPatchpoint(env, instr);
      return;
    case Instruction::kLoadThreadState:
      translateLoadThreadState(env, instr);
      return;
    case Instruction::kYieldInitial:
      translateYieldInitial(env, instr);
      return;
    case Instruction::kYieldValue:
      translateYieldValue(env, instr);
      return;
    case Instruction::kStoreGenYieldPoint:
      translateStoreGenYieldPoint(env, instr);
      return;
    case Instruction::kStoreGenYieldFromPoint:
      translateStoreGenYieldFromPoint(env, instr);
      return;
    case Instruction::kBranchToYieldExit:
      translateBranchToYieldExit(env, instr);
      return;
    case Instruction::kResumeGenYield:
      translateResumeGenYield(env, instr);
      return;
    case Instruction::kYieldExitPoint:
      translateYieldExitPoint(env, instr);
      return;
    case Instruction::kEpilogueEnd:
      translateEpilogueEnd(env, instr);
      return;
    case Instruction::kIntToBool:
      translateIntToBool(env, instr);
      return;
    case Instruction::kPrologue:
      translatePrologue(env, instr);
      return;
    case Instruction::kSetupFrame:
      translateSetupFrame(env, instr);
      return;
    case Instruction::kIndirectJump:
      translateIndirectJump(env, instr);
      return;
    case Instruction::kInc:
      translateInc(env, instr);
      return;
    case Instruction::kDec:
      translateDec(env, instr);
      return;
    case Instruction::kBitTest:
      translateBitTest(env, instr);
      return;
    case Instruction::kSelect:
      translateSelect(env, instr);
      return;
    case Instruction::kEqual:
    case Instruction::kNotEqual:
    case Instruction::kGreaterThanUnsigned:
    case Instruction::kGreaterThanEqualUnsigned:
    case Instruction::kLessThanUnsigned:
    case Instruction::kLessThanEqualUnsigned:
    case Instruction::kGreaterThanSigned:
    case Instruction::kGreaterThanEqualSigned:
    case Instruction::kLessThanSigned:
    case Instruction::kLessThanEqualSigned:
      TranslateCompare(env, instr);
      return;
    case Instruction::kFadd: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fadd(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fadd(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Instruction::kFsub: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fsub(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fsub(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Instruction::kFmul: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fmul(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fmul(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Instruction::kFdiv: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fdiv(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fdiv(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Instruction::kInt64ToDouble:
      env->as->scvtf(
          getVecD(instr->output()), getReg(instr, instr->getInput(0)));
      return;
    case Instruction::kExchange:
      translateExchange(env, instr);
      return;
    case Instruction::kCmp:
      translateCmp(env, instr);
      return;
    case Instruction::kNegate:
      translateNegate(env, instr);
      return;
    case Instruction::kInvert:
      translateInvert(env, instr);
      return;
    case Instruction::kAdd:
      translateAdd(env, instr);
      return;
    case Instruction::kSub:
      translateSub(env, instr);
      return;
    case Instruction::kAnd:
      translateAnd(env, instr);
      return;
    case Instruction::kOr:
      translateOr(env, instr);
      return;
    case Instruction::kXor:
      translateXor(env, instr);
      return;
    case Instruction::kMul:
      translateMul(env, instr);
      return;
    case Instruction::kTest32: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->tst(
          asmjit::a64::w(in0->getPhyRegister().loc),
          asmjit::a64::w(in1->getPhyRegister().loc));
      return;
    }
    case Instruction::kCall:
      translateCall(env, instr);
      return;
    case Instruction::kMove:
      translateMove(env, instr);
      return;
    case Instruction::kMovConstPool:
      translateMovConstPool(env, instr);
      return;
    case Instruction::kMulAdd:
      translateMulAdd(env, instr);
      return;
    case Instruction::kNone:
    case Instruction::kNop:
    case Instruction::kVectorCall:
    case Instruction::kVarArgCall:
    case Instruction::kSext:
    case Instruction::kZext:
    case Instruction::kCdq:
    case Instruction::kCwd:
    case Instruction::kCqo:
    case Instruction::kLShift:
    case Instruction::kRShift:
    case Instruction::kRShiftUn:
    case Instruction::kLoadArg:
    case Instruction::kLoadSecondCallResult:
    case Instruction::kCondBranch:
    case Instruction::kPhi:
    case Instruction::kReturn:
      JIT_ABORT("Unexpected opcode {} in translateInstr", (int)opcode);
#endif
    default:
      JIT_ABORT(
          "No handler for opcode {}", InstrProperty::getProperties(instr).name);
  }
}

namespace {

void fillLiveValueLocations(
    CodeRuntime* code_runtime,
    std::size_t deopt_idx,
    const Instruction* instr,
    size_t begin_input,
    size_t end_input) {
  ThreadedCompileSerialize guard;

  DeoptMetadata& deopt_meta = code_runtime->getDeoptMetadata(deopt_idx);
  for (size_t i = begin_input; i < end_input; i++) {
    auto loc = instr->getInput(i)->getPhyRegOrStackSlot();
    deopt_meta.live_values[i - begin_input].location = loc;
  }
}

} // namespace

// Translate GUARD instruction
void TranslateGuard(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  auto as = env->as;

  // the first four operands of the guard instruction are:
  //   * kind
  //   * deopt meta id
  //   * guard var (physical register) (0 for AlwaysFail)
  //   * target (for GuardIs and GuardType, and 0 for all others)

  auto deopt_label = as->newLabel();
  auto kind = instr->getInput(0)->getConstant();

  arch::Gp reg = x86::rax;
  bool is_double = false;
  if (kind != kAlwaysFail) {
    if (instr->getInput(2)->dataType() == jit::lir::OperandBase::kDouble) {
      JIT_CHECK(kind == kNotZero, "Only NotZero is supported for double");
      auto vecd_reg = AutoTranslator::getVecD(instr->getInput(2));
      as->ptest(vecd_reg, vecd_reg);
      as->jz(deopt_label);
      is_double = true;
    } else {
      reg = AutoTranslator::getGp(instr->getInput(2));
    }
  }

  auto emit_cmp = [&](auto reg_arg) {
    constexpr size_t kTargetIndex = 3;
    auto target_opnd = instr->getInput(kTargetIndex);
    if (target_opnd->isImm() || target_opnd->isMem()) {
      auto target = target_opnd->getConstantOrAddress();
      JIT_DCHECK(
          fitsSignedInt<32>(target),
          "Constant operand should fit in a 32-bit register, got {:x}.",
          target);
      as->cmp(reg_arg, target);
    } else {
      auto target_reg = AutoTranslator::getGp(target_opnd);
      as->cmp(reg_arg, target_reg);
    }
  };

  if (!is_double) {
    switch (kind) {
      case kNotZero: {
        as->test(reg, reg);
        as->jz(deopt_label);
        break;
      }
      case kNotNegative: {
        as->test(reg, reg);
        as->js(deopt_label);
        break;
      }
      case kZero: {
        as->test(reg, reg);
        as->jnz(deopt_label);
        break;
      }
      case kAlwaysFail:
        as->jmp(deopt_label);
        break;
      case kIs:
        emit_cmp(reg);
        as->jne(deopt_label);
        break;
      case kHasType: {
        emit_cmp(x86::qword_ptr(reg, offsetof(PyObject, ob_type)));
        as->jne(deopt_label);
        break;
      }
    }
  }
#elif defined(CINDER_AARCH64)
  auto as = env->as;

  // the first four operands of the guard instruction are:
  //   * kind
  //   * deopt meta id
  //   * guard var (physical register) (0 for AlwaysFail)
  //   * target (for GuardIs and GuardType, and 0 for all others)

  auto deopt_label = as->newLabel();
  auto kind = instr->getInput(0)->getConstant();

  arch::Gp reg = arch::reg_scratch_0;
  uint64_t mask = 0;
  size_t sign_bit = 0;
  if (kind != kAlwaysFail) {
    auto data_type = instr->getInput(2)->dataType();
    if (data_type == jit::lir::OperandBase::k8bit) {
      mask = 0xFF;
      sign_bit = 7;
      // aarch64 doesn't have 8-bit registers, use 32-bit w register.
      reg = asmjit::a64::w(instr->getInput(2)->getPhyRegister().loc);
    } else if (data_type == jit::lir::OperandBase::k16bit) {
      mask = 0xFFFF;
      sign_bit = 15;
      // aarch64 doesn't have 16-bit registers, use 32-bit w register.
      reg = asmjit::a64::w(instr->getInput(2)->getPhyRegister().loc);
    } else {
      reg = AutoTranslator::getGp(instr->getInput(2));
      sign_bit = reg.size() * CHAR_BIT - 1;
    }
  }

  auto emit_cmp = [&](auto reg_arg) {
    constexpr size_t kTargetIndex = 3;
    auto target_opnd = instr->getInput(kTargetIndex);
    if (target_opnd->isImm() || target_opnd->isMem()) {
      auto target = target_opnd->getConstantOrAddress();
      arch::cmp_immediate(as, reg_arg, target);
    } else {
      auto target_reg = AutoTranslator::getGpWiden(target_opnd);
      as->cmp(reg_arg, target_reg);
    }
  };

  switch (kind) {
    case kNotZero:
      if (mask) {
        as->tst(reg, mask);
        as->b_eq(deopt_label);
      } else {
        as->cbz(reg, deopt_label);
      }
      break;
    case kNotNegative:
      as->tbnz(reg, sign_bit, deopt_label);
      break;
    case kZero:
      if (mask) {
        as->tst(reg, mask);
        as->b_ne(deopt_label);
      } else {
        as->cbnz(reg, deopt_label);
      }
      break;
    case kAlwaysFail:
      as->b(deopt_label);
      break;
    case kIs:
      emit_cmp(reg);
      as->b_ne(deopt_label);
      break;
    case kHasType:
      JIT_ABORT(
          "kHasType should have been lowered to kIs by postgen "
          "rewriteGuardHasType");
  }
#else
  CINDER_UNSUPPORTED
#endif

  auto index = instr->getInput(1)->getConstant();
  // skip the first four inputs in Guard, which are
  // kind, deopt_meta id, guard var, and target.
  fillLiveValueLocations(env->code_rt, index, instr, 4, instr->getNumInputs());
  env->deopt_exits.emplace_back(index, deopt_label, instr);
}

void TranslateDeoptPatchpoint(Environ* env, const Instruction* instr) {
  auto as = env->as;

  auto patcher =
      reinterpret_cast<JumpPatcher*>(instr->getInput(0)->getMemoryAddress());

  // Generate patchpoint by writing in an appropriately sized nop.  As a future
  // optimization, we may be able to avoid reserving space for the patchpoint if
  // we can prove that the following bytes are not the target of a jump.
#if defined(CINDER_X86_64) && defined(Py_GIL_DISABLED)
  // On x86, align the patchpoint to 8 bytes so the patch-point doesn't straddle
  // a cache line boundary. This is enough to make updates appear atomic to
  // other cores.
  //
  // Not needed on Arm as fixed instructions are a fixed size and updates
  // naturally atomic.
  as->align(AlignMode::kCode, 8);
#endif
  auto patchpoint_label = as->newLabel();
  as->bind(patchpoint_label);

  auto stored_bytes = patcher->storedBytes();
  as->embed(stored_bytes.data(), stored_bytes.size());

  // Fill in deopt metadata
  auto index = instr->getInput(1)->getConstant();
  // skip the first two inputs which are the patcher and deopt metadata id
  fillLiveValueLocations(env->code_rt, index, instr, 2, instr->getNumInputs());
  auto deopt_label = as->newLabel();
  env->deopt_exits.emplace_back(index, deopt_label, instr);

  // The runtime will link the patcher to the appropriate point in the code
  // once code generation has completed.
  env->pending_deopt_patchers.emplace_back(
      patcher, patchpoint_label, deopt_label);
}

void TranslateCompare(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  auto as = env->as;
  const OperandBase* inp0 = instr->getInput(0);
  const OperandBase* inp1 = instr->getInput(1);

  if (inp1->isImm() || inp1->isMem()) {
    as->cmp(AutoTranslator::getGp(inp0), inp1->getConstantOrAddress());
  } else if (!inp1->isVecD()) {
    as->cmp(AutoTranslator::getGp(inp0), AutoTranslator::getGp(inp1));
  } else {
    as->comisd(AutoTranslator::getVecD(inp0), AutoTranslator::getVecD(inp1));
  }
  auto output = AutoTranslator::getGp(instr->output());
  switch (instr->opcode()) {
    case Instruction::kEqual:
      as->sete(output);
      break;
    case Instruction::kNotEqual:
      as->setne(output);
      break;
    case Instruction::kGreaterThanSigned:
      as->setg(output);
      break;
    case Instruction::kGreaterThanEqualSigned:
      as->setge(output);
      break;
    case Instruction::kLessThanSigned:
      as->setl(output);
      break;
    case Instruction::kLessThanEqualSigned:
      as->setle(output);
      break;
    case Instruction::kGreaterThanUnsigned:
      as->seta(output);
      break;
    case Instruction::kGreaterThanEqualUnsigned:
      as->setae(output);
      break;
    case Instruction::kLessThanUnsigned:
      as->setb(output);
      break;
    case Instruction::kLessThanEqualUnsigned:
      as->setbe(output);
      break;
    default:
      JIT_ABORT("bad instruction for TranslateCompare");
  }
  if (instr->output()->dataType() != OperandBase::k8bit) {
    as->movzx(
        AutoTranslator::getGp(instr->output()),
        asmjit::x86::gpb(instr->output()->getPhyRegister().loc));
  }
#elif defined(CINDER_AARCH64)
  auto as = env->as;
  const OperandBase* inp0 = instr->getInput(0);
  const OperandBase* inp1 = instr->getInput(1);

  if (inp1->isMem()) {
    JIT_CHECK(inp1->sizeInBits() == 64, "Only 64-bit memory supported");

    auto address = inp1->getConstantOrAddress();
    auto scratch = arch::reg_scratch_0;

    as->mov(scratch, address);
    as->ldr(scratch, a64::ptr(scratch));
    as->cmp(AutoTranslator::getGpWiden(inp0), scratch);
  } else if (inp1->isImm()) {
    auto constant = inp1->getConstantOrAddress();
    arch::cmp_immediate(as, AutoTranslator::getGpWiden(inp0), constant);
  } else if (!inp1->isVecD()) {
    as->cmp(AutoTranslator::getGpWiden(inp0), AutoTranslator::getGpWiden(inp1));
  } else {
    as->fcmp(AutoTranslator::getVecD(inp0), AutoTranslator::getVecD(inp1));
  }

  auto output = AutoTranslator::getGpOutput(instr->output());
  switch (instr->opcode()) {
    case Instruction::kEqual:
      as->cset(output, arm::CondCode::kEQ);
      break;
    case Instruction::kNotEqual:
      as->cset(output, arm::CondCode::kNE);
      break;
    case Instruction::kGreaterThanSigned:
      as->cset(output, arm::CondCode::kGT);
      break;
    case Instruction::kGreaterThanEqualSigned:
      as->cset(output, arm::CondCode::kGE);
      break;
    case Instruction::kLessThanSigned:
      as->cset(output, arm::CondCode::kLT);
      break;
    case Instruction::kLessThanEqualSigned:
      as->cset(output, arm::CondCode::kLE);
      break;
    case Instruction::kGreaterThanUnsigned:
      as->cset(output, arm::CondCode::kHI);
      break;
    case Instruction::kGreaterThanEqualUnsigned:
      as->cset(output, arm::CondCode::kHS);
      break;
    case Instruction::kLessThanUnsigned:
      as->cset(output, arm::CondCode::kLO);
      break;
    case Instruction::kLessThanEqualUnsigned:
      as->cset(output, arm::CondCode::kLS);
      break;
    default:
      JIT_ABORT("bad instruction for TranslateCompare");
  }
#else
  CINDER_UNSUPPORTED
#endif
}

void translateIntToBool(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  x86::Builder* as = env->as;
  const OperandBase* input = instr->getInput(0);
  x86::Gp output = AutoTranslator::getGp(instr->output());
  JIT_CHECK(
      instr->output()->dataType() == OperandBase::k8bit,
      "Output should be 8bits, not {}",
      instr->output()->dataType());
  if (input->isImm()) {
    as->mov(output, input->getConstant() ? 1 : 0);
  } else {
    as->test(AutoTranslator::getGp(input), AutoTranslator::getGp(input));
    as->setne(output);
  }
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;
  const OperandBase* input = instr->getInput(0);
  a64::Gp output = AutoTranslator::getGpOutput(instr->output());
  JIT_CHECK(
      instr->output()->dataType() == OperandBase::k8bit,
      "Output should be 8bits, not {}",
      instr->output()->dataType());
  as->cmp(AutoTranslator::getGpWiden(input), 0);
  as->cset(output, a64::CondCode::kNE);
#else
  CINDER_UNSUPPORTED
#endif
}

// Store meta-data about this yield in a generator suspend data pointed to by
// suspend_data_r. Data includes things like the address to resume execution at,
// and owned entries in the suspended spill data needed for GC operations etc.
void emitStoreGenYieldPoint(
    arch::Builder* as,
    Environ* env,
    const Instruction* yield,
    asmjit::Label resume_label,
    arch::Gp suspend_data_r,
    arch::Gp scratch_r,
    bool is_yield_from) {
  auto calc_spill_offset = [&](size_t live_input_n) {
    PhyLocation mem = yield->getInput(live_input_n)->getStackSlot();
    return mem.loc / kPointerSize;
  };

  size_t input_n = yield->getNumInputs() - 1;
  size_t deopt_idx = yield->getInput(input_n)->getConstant();

  size_t live_regs_input = input_n - 1;
  int num_live_regs = yield->getInput(live_regs_input)->getConstant();
  fillLiveValueLocations(
      env->code_rt,
      deopt_idx,
      yield,
      live_regs_input - num_live_regs,
      live_regs_input);

  auto yield_from_offset =
      is_yield_from ? calc_spill_offset(0) : kInvalidYieldFromOffset;
  GenYieldPoint* gen_yield_point = env->code_rt->addGenYieldPoint(
      GenYieldPoint{deopt_idx, yield_from_offset});

  env->unresolved_gen_entry_labels.emplace(gen_yield_point, resume_label);
  if (yield->origin()) {
    env->pending_debug_locs.emplace_back(resume_label, yield->origin());
  }

  as->mov(scratch_r, reinterpret_cast<uint64_t>(gen_yield_point));
  auto yieldPointOffset = offsetof(GenDataFooter, yieldPoint);

#if defined(CINDER_X86_64)
  as->mov(x86::qword_ptr(suspend_data_r, yieldPointOffset), scratch_r);
#elif defined(CINDER_AARCH64)
  as->str(
      scratch_r,
      arch::ptr_resolve(
          as, suspend_data_r, yieldPointOffset, arch::reg_scratch_0));
#else
  (void)yieldPointOffset;
  CINDER_UNSUPPORTED
#endif
}

void emitLoadResumedYieldInputs(
    arch::Builder* as,
    const Instruction* instr,
    PhyLocation sent_in_source_loc,
    arch::Gp tstate_reg) {
#if defined(CINDER_X86_64)
  PhyLocation tstate = instr->getInput(0)->getStackSlot();
  as->mov(x86::ptr(x86::rbp, tstate.loc), tstate_reg);

  const lir::Operand* target = instr->output();

  if (target->isStack()) {
    as->mov(
        x86::ptr(x86::rbp, target->getStackSlot().loc),
        x86::gpq(sent_in_source_loc.loc));
    return;
  }

  if (target->isReg()) {
    PhyLocation target_loc = target->getPhyRegister();
    if (target_loc != sent_in_source_loc) {
      as->mov(x86::gpq(target_loc.loc), x86::gpq(sent_in_source_loc.loc));
    }
    return;
  }

  JIT_CHECK(
      target->isNone(),
      "Have an output that isn't a register or a stack slot, {}",
      target->type());
#elif defined(CINDER_AARCH64)
  PhyLocation tstate = instr->getInput(0)->getStackSlot();
  as->str(
      tstate_reg,
      arch::ptr_resolve(as, arch::fp, tstate.loc, arch::reg_scratch_0));

  const lir::Operand* target = instr->output();

  if (target->isStack()) {
    as->str(
        a64::x(sent_in_source_loc.loc),
        arch::ptr_resolve(
            as, arch::fp, target->getStackSlot().loc, arch::reg_scratch_0));
    return;
  }

  if (target->isReg()) {
    PhyLocation target_loc = target->getPhyRegister();
    if (target_loc != sent_in_source_loc) {
      as->mov(a64::x(target_loc.loc), a64::x(sent_in_source_loc.loc));
    }
    return;
  }

  JIT_CHECK(
      target->isNone(),
      "Have an output that isn't a register or a stack slot, {}",
      target->type());
#else
  CINDER_UNSUPPORTED
#endif
}

void translateLoadThreadState(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  const lir::Operand* output = instr->output();

#if defined(CINDER_X86_64)
  x86::Gp dst;
  if (output->isReg()) {
    dst = x86::gpq(output->getPhyRegister().loc);
  } else if (output->isStack()) {
    // Use rax as scratch, will store to stack slot afterwards.
    dst = x86::rax;
  } else {
    JIT_ABORT("LoadThreadState output must be a register or stack slot");
  }

#if PY_VERSION_HEX >= 0x030C0000
  if (cinderx::getModuleState()->tstate_offset != -1) {
    // Fast path: load tstate directly from the TLS segment register.
    asmjit::x86::Mem tls(cinderx::getModuleState()->tstate_offset);
    tls.setSegment(x86::fs);
    as->mov(dst, tls);
  } else {
    // Fallback: call _PyThreadState_GetCurrent().
    as->call(_PyThreadState_GetCurrent);
    if (dst.id() != x86::rax.id()) {
      as->mov(dst, x86::rax);
    }
  }
#else
  // 3.10: Load from _PyRuntime.gilstate.tstate_current.
  uint64_t tstate_addr =
      reinterpret_cast<uint64_t>(&_PyRuntime.gilstate.tstate_current);
  if (fitsSignedInt<32>(tstate_addr)) {
    as->mov(dst, x86::ptr(tstate_addr));
  } else {
    as->mov(dst, tstate_addr);
    as->mov(dst, x86::ptr(dst));
  }
#endif

  if (output->isStack()) {
    as->mov(x86::ptr(x86::rbp, output->getStackSlot().loc), dst);
  }

#elif defined(CINDER_AARCH64)
  a64::Gp dst;
  if (output->isReg()) {
    dst = a64::x(output->getPhyRegister().loc);
  } else if (output->isStack()) {
    dst = a64::x0;
  } else {
    JIT_ABORT("LoadThreadState output must be a register or stack slot");
  }

#if PY_VERSION_HEX >= 0x030C0000
  if (cinderx::getModuleState()->tstate_offset != -1) {
    // Fast path: load tstate from thread-local storage.
    as->mrs(dst, a64::Predicate::SysReg::kTPIDR_EL0);
    as->ldr(
        dst,
        arch::ptr_resolve(
            as,
            dst,
            cinderx::getModuleState()->tstate_offset,
            arch::reg_scratch_0));
  } else {
    // Fallback: call _PyThreadState_GetCurrent().
    as->mov(arch::reg_scratch_br, _PyThreadState_GetCurrent);
    as->blr(arch::reg_scratch_br);
    if (dst.id() != a64::x0.id()) {
      as->mov(dst, a64::x0);
    }
  }
#else
  // 3.10: Load from _PyRuntime.gilstate.tstate_current.
  uint64_t tstate_addr =
      reinterpret_cast<uint64_t>(&_PyRuntime.gilstate.tstate_current);
  as->mov(dst, tstate_addr);
  as->ldr(dst, a64::ptr(dst));
#endif

  if (output->isStack()) {
    as->str(
        dst,
        arch::ptr_resolve(
            as, arch::fp, output->getStackSlot().loc, arch::reg_scratch_0));
  }

#else
  CINDER_UNSUPPORTED
#endif
}

void translateYieldInitial(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
#if PY_VERSION_HEX < 0x030C0000
  arch::Builder* as = env->as;

  // Load tstate into RDI for call to JITRT_MakeGenObject*.

  // Consider avoiding reloading the tstate in from memory if it was already in
  // a register before spilling. Still needs to be in memory though so it can be
  // recovered after calling JITRT_MakeGenObject* which will trash it.
  PhyLocation tstate = instr->getInput(0)->getStackSlot();
  as->mov(x86::rdi, x86::ptr(x86::rbp, tstate.loc));

  // Make a generator object to be returned by the epilogue.
  as->lea(x86::rsi, x86::ptr(env->gen_resume_entry_label));
  JIT_CHECK(
      env->shadow_frames_and_spill_size % kPointerSize == 0,
      "Bad spill alignment");
  as->mov(x86::rdx, env->shadow_frames_and_spill_size / kPointerSize);
  as->mov(x86::rcx, reinterpret_cast<uint64_t>(env->code_rt));
  JIT_CHECK(instr->origin()->IsInitialYield(), "expected InitialYield");
  PyCodeObject* code = static_cast<const hir::InitialYield*>(instr->origin())
                           ->frameState()
                           ->code;
  as->mov(x86::r8, reinterpret_cast<uint64_t>(code));
  if (code->co_flags & CO_COROUTINE) {
    emitCall(*env, reinterpret_cast<uint64_t>(JITRT_MakeGenObjectCoro), instr);
  } else if (code->co_flags & CO_ASYNC_GENERATOR) {
    emitCall(
        *env, reinterpret_cast<uint64_t>(JITRT_MakeGenObjectAsyncGen), instr);
  } else {
    emitCall(*env, reinterpret_cast<uint64_t>(JITRT_MakeGenObject), instr);
  }
  // Resulting generator is now in RAX for filling in below and epilogue return.
  const auto gen_reg = x86::rax;

  // Exit early if return from JITRT_MakeGenObject was nullptr.
  as->test(gen_reg, gen_reg);
  as->jz(env->hard_exit_label);

  // Set RDI to gen->gi_jit_data for use in emitStoreGenYieldPoint() and data
  // copy using 'movsq' below.
  auto gi_jit_data_offset = offsetof(PyGenObject, gi_jit_data);
  as->mov(x86::rdi, x86::ptr(gen_reg, gi_jit_data_offset));

  // Arbitrary scratch register for use in emitStoreGenYieldPoint().
  auto scratch_r = x86::r9;
  asmjit::Label resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as, env, instr, resume_label, x86::rdi, scratch_r, false);

  // Store variables spilled by this point to generator.
  int spill_bytes = env->initial_yield_spill_size_;
  JIT_CHECK(spill_bytes % kPointerSize == 0, "Bad spill alignment");

  // Point rsi at the bottom word of the current spill space.
  as->lea(x86::rsi, x86::ptr(x86::rbp, -spill_bytes));
  // Point rdi at the bottom word of the generator's spill space.
  as->sub(x86::rdi, spill_bytes);
  as->mov(x86::rcx, spill_bytes / kPointerSize);
  as->rep().movsq();

  // Jump to bottom half of epilogue
  as->jmp(env->hard_exit_label);

  // Resumed execution in this generator begins here
  as->bind(resume_label);

  // Sent in value is in RSI, and tstate is in RCX from resume entry-point args
  emitLoadResumedYieldInputs(as, instr, RSI, x86::rcx);
#else
  arch::Builder* as = env->as;

  // Load tstate into RDI for call to
  // JITRT_UnlinkGenFrameAndReturnGenDataFooter.

  // Consider avoiding reloading the tstate in from memory if it was already in
  // a register before spilling. Still needs to be in memory though so it can be
  // recovered after calling JITRT_MakeGenObject* which will trash it.
  PhyLocation tstate = instr->getInput(0)->getStackSlot();
  as->mov(x86::rdi, x86::ptr(x86::rbp, tstate.loc));

  emitCall(
      *env,
      reinterpret_cast<uint64_t>(JITRT_UnlinkGenFrameAndReturnGenDataFooter),
      instr);
  // This will return pointers to a generator in RAX and JIT data in RDX.

  // Arbitrary scratch register for use in emitStoreGenYieldPoint(). Any
  // caller-saved register not used in this scope will do because we're on the
  // exit path now.
  auto scratch_r = x86::r9;
  asmjit::Label resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as, env, instr, resume_label, x86::rdx, scratch_r, false);

  // Jump to epilogue
  as->jmp(env->exit_for_yield_label);

  // Resumed execution in this generator begins here
  as->bind(resume_label);

  // Sent in value is in RSI, and tstate is in RCX from resume entry-point args
  emitLoadResumedYieldInputs(as, instr, RSI, x86::rcx);
#endif
#elif defined(CINDER_AARCH64)
#if PY_VERSION_HEX < 0x030C0000
  CINDER_UNSUPPORTED
#else
  arch::Builder* as = env->as;

  // Load tstate into X0 for call to
  // JITRT_UnlinkGenFrameAndReturnGenDataFooter.

  // Consider avoiding reloading the tstate in from memory if it was already in
  // a register before spilling. Still needs to be in memory though so it can be
  // recovered after calling JITRT_MakeGenObject* which will trash it.
  PhyLocation tstate = instr->getInput(0)->getStackSlot();
  as->ldr(
      a64::x0,
      arch::ptr_resolve(as, arch::fp, tstate.loc, arch::reg_scratch_0));

  emitCall(
      *env,
      reinterpret_cast<uint64_t>(JITRT_UnlinkGenFrameAndReturnGenDataFooter),
      instr);
  // This will return pointers to a generator in X0 and JIT data in X1.

  // Arbitrary scratch register for use in emitStoreGenYieldPoint(). Any
  // caller-saved register not used in this scope will do because we're on the
  // exit path now.
  auto scratch_r = arch::reg_scratch_0;
  asmjit::Label resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as, env, instr, resume_label, a64::x1, scratch_r, false);

  // Jump to epilogue
  as->b(env->exit_for_yield_label);

  // Resumed execution in this generator begins here
  as->bind(resume_label);

  // Sent in value is in X1, and tstate is in X3 from resume entry-point args
  emitLoadResumedYieldInputs(as, instr, X1, a64::x3);
#endif
#else
  CINDER_UNSUPPORTED
#endif
}

void translateYieldValue(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  arch::Builder* as = env->as;

  // Value to send goes to RAX so it can be yielded (returned) by epilogue.
  if (instr->getInput(1)->isImm()) {
    as->mov(x86::rax, instr->getInput(1)->getConstant());
  } else {
    PhyLocation value_out = instr->getInput(1)->getStackSlot();
    as->mov(x86::rax, x86::ptr(x86::rbp, value_out.loc));
  }
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;

  // Value to send goes to x0 so it can be yielded (returned) by epilogue.
  if (instr->getInput(1)->isImm()) {
    as->mov(a64::x0, instr->getInput(1)->getConstant());
  } else {
    PhyLocation value_out = instr->getInput(1)->getStackSlot();
    as->ldr(
        a64::x0,
        arch::ptr_resolve(as, arch::fp, value_out.loc, arch::reg_scratch_0));
  }
#else
  CINDER_UNSUPPORTED
#endif
}

void translateStoreGenYieldPoint(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  arch::Builder* as = env->as;
  auto scratch_r = x86::r9;
  env->pending_yield_resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as,
      env,
      instr,
      env->pending_yield_resume_label,
      x86::rbp,
      scratch_r,
      false);
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;
  auto scratch_r = arch::reg_scratch_0;
  env->pending_yield_resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as,
      env,
      instr,
      env->pending_yield_resume_label,
      arch::fp,
      scratch_r,
      false);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateStoreGenYieldFromPoint(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  arch::Builder* as = env->as;
  auto scratch_r = x86::r9;
  env->pending_yield_resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as,
      env,
      instr,
      env->pending_yield_resume_label,
      x86::rbp,
      scratch_r,
      true);
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;
  auto scratch_r = arch::reg_scratch_0;
  env->pending_yield_resume_label = as->newLabel();
  emitStoreGenYieldPoint(
      as,
      env,
      instr,
      env->pending_yield_resume_label,
      arch::fp,
      scratch_r,
      true);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateBranchToYieldExit(Environ* env, const Instruction*) {
#if defined(CINDER_X86_64)
  env->as->jmp(env->exit_for_yield_label);
#elif defined(CINDER_AARCH64)
  env->as->b(env->exit_for_yield_label);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateResumeGenYield(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  arch::Builder* as = env->as;

  // Resumed execution in this generator begins here
  as->bind(env->pending_yield_resume_label);

#if PY_VERSION_HEX < 0x030C0000
  // On 3.10, for yield-from yield points, store the finish_yield_from arg
  // (RDX from resume entry) into GenDataFooter so the subsequent Send
  // instruction can load it.
  if (instr->origin() &&
      static_cast<const hir::YieldValue*>(instr->origin())->isYieldFrom()) {
    auto fyf_offset = offsetof(GenDataFooter, finishYieldFrom);
    as->mov(x86::qword_ptr(x86::rbp, fyf_offset), x86::rdx);
  }
#endif

  // Sent in value is in RSI, and tstate is in RCX from resume entry-point args
  emitLoadResumedYieldInputs(as, instr, RSI, x86::rcx);
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;

  // Resumed execution in this generator begins here
  as->bind(env->pending_yield_resume_label);

#if PY_VERSION_HEX < 0x030C0000
  // On 3.10, for yield-from yield points, store the finish_yield_from arg
  // (X2 from resume entry) into GenDataFooter so the subsequent Send
  // instruction can load it.
  if (instr->origin() &&
      static_cast<const hir::YieldValue*>(instr->origin())->isYieldFrom()) {
    auto fyf_offset = offsetof(GenDataFooter, finishYieldFrom);
    as->str(
        a64::x2,
        arch::ptr_resolve(as, arch::fp, fyf_offset, arch::reg_scratch_0));
  }
#endif

  // Sent in value is in x1, and tstate is in x3 from resume entry-point args
  emitLoadResumedYieldInputs(as, instr, X1, a64::x3);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateYieldExitPoint(Environ* env, const Instruction*) {
  env->as->bind(env->exit_for_yield_label);
}

void translateLeaLabel(Environ* env, const Instruction* instr) {
  auto* as = env->as;
  auto output = instr->output();
  auto* input = instr->getInput(0);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(input->isLabel(), "Expected input to be a label");

  asmjit::Label label = input->getDefine()->hasAsmLabel()
      ? input->getDefine()->getAsmLabel()
      : map_get(env->block_label_map, input->getBasicBlock());

#if defined(CINDER_X86_64)
  as->lea(x86::gpq(output->getPhyRegister().loc), x86::ptr(label));
#elif defined(CINDER_AARCH64)
  as->adr(a64::x(output->getPhyRegister().loc), label);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateEpilogueEnd(Environ* env, const Instruction* instr) {
  auto* as = env->as;

  auto* ret_val = instr->getInput(0);
  bool is_primitive = ret_val->dataType() != DataType::kObject;
  bool is_double = ret_val->isFp();

#if defined(CINDER_X86_64)
  // Move return value to ABI return register
  if (is_double) {
    if (ret_val->isStack()) {
      as->movsd(x86::xmm0, x86::ptr(x86::rbp, ret_val->getStackSlot().loc));
    } else if (
        ret_val->isReg() &&
        ret_val->getPhyRegister().loc != arch::reg_double_return_loc.loc) {
      as->movsd(
          x86::xmm0, x86::xmm(ret_val->getPhyRegister().loc - VECD_REG_BASE));
    }
  } else {
    if (ret_val->isStack()) {
      as->mov(x86::rax, x86::ptr(x86::rbp, ret_val->getStackSlot().loc));
    } else if (
        ret_val->isReg() &&
        ret_val->getPhyRegister().loc != arch::reg_general_return_loc.loc) {
      as->mov(x86::rax, x86::gpq(ret_val->getPhyRegister().loc));
    }
  }

  if (is_primitive) {
    if (is_double) {
      as->pcmpeqw(x86::xmm1, x86::xmm1);
      as->psrlq(x86::xmm1, 63);
    } else {
      as->mov(x86::edx, 1);
    }
  }

  as->bind(env->hard_exit_label);
  auto saved_regs = env->changed_regs & CALLEE_SAVE_REGS;
  if (!saved_regs.Empty()) {
    JIT_CHECK(
        env->last_callee_saved_reg_off != -1,
        "offset to callee saved regs not initialized");
    // Point rsp at the bottom of the callee-saved area, then pop in
    // reverse push order (GetLast→GetFirst) to restore registers.
    as->lea(x86::rsp, x86::ptr(x86::rbp, -env->last_callee_saved_reg_off));
    while (!saved_regs.Empty()) {
      as->pop(x86::gpq(saved_regs.GetLast().loc));
      saved_regs.RemoveLast();
    }
  }
  as->leave();
  as->ret();
#elif defined(CINDER_AARCH64)
  // Move return value to ABI return register
  if (is_double) {
    if (ret_val->isStack()) {
      as->ldr(a64::d0, a64::ptr(arch::fp, ret_val->getStackSlot().loc));
    } else if (
        ret_val->isReg() &&
        ret_val->getPhyRegister().loc != arch::reg_double_return_loc.loc) {
      as->fmov(a64::d0, a64::d(ret_val->getPhyRegister().loc - VECD_REG_BASE));
    }
  } else {
    if (ret_val->isStack()) {
      as->ldr(a64::x0, a64::ptr(arch::fp, ret_val->getStackSlot().loc));
    } else if (
        ret_val->isReg() &&
        ret_val->getPhyRegister().loc != arch::reg_general_return_loc.loc) {
      as->mov(a64::x0, a64::x(ret_val->getPhyRegister().loc));
    }
  }

  if (is_primitive) {
    if (is_double) {
      as->fmov(a64::d1, 1.0);
    } else {
      as->mov(a64::w1, 1);
    }
  }

  as->bind(env->hard_exit_label);
  auto saved_regs = env->changed_regs & CALLEE_SAVE_REGS;
  if (!saved_regs.Empty()) {
    JIT_CHECK(
        env->last_callee_saved_reg_off != -1,
        "offset to callee saved regs not initialized");
    JIT_CHECK(env->last_callee_saved_reg_off % kStackAlign == 0, "unaligned");
    // Restore callee-saved registers from fixed offsets below FP.
    // Use a scratch register as base to avoid large FP-relative offsets
    // that can exceed arm64 ldp/ldr encoding range.
    auto gp_regs = saved_regs & ALL_GP_REGISTERS;
    auto vecd_regs = saved_regs & ALL_VECD_REGISTERS;

    int gp_size = (((gp_regs.count() + 1) / 2)) * kStackAlign;
    int vecd_size = (((vecd_regs.count() + 1) / 2)) * kStackAlign;
    int header_and_spill_size =
        env->last_callee_saved_reg_off - gp_size - vecd_size;

    // base = fp - header_and_spill_size (points to start of callee-saved area)
    arch::sub_immediate(
        as,
        arch::reg_scratch_0,
        arch::fp,
        static_cast<uint64_t>(header_and_spill_size));

    // Restore GP registers (iterate GetFirst→GetLast, same as save).
    int offset = 0;
    if (!gp_regs.Empty()) {
      if (gp_regs.count() % 2 == 1) {
        as->ldr(
            a64::x(gp_regs.GetFirst().loc),
            a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        gp_regs.RemoveFirst();
        offset += 16;
      }
      while (!gp_regs.Empty()) {
        auto first = a64::x(gp_regs.GetFirst().loc);
        gp_regs.RemoveFirst();
        auto second = a64::x(gp_regs.GetFirst().loc);
        gp_regs.RemoveFirst();
        as->ldp(first, second, a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        offset += 16;
      }
    }

    // Restore VecD registers (iterate GetFirst→GetLast, same as save).
    if (!vecd_regs.Empty()) {
      if (vecd_regs.count() % 2 == 1) {
        as->ldr(
            a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
            a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        vecd_regs.RemoveFirst();
        offset += 16;
      }
      while (!vecd_regs.Empty()) {
        auto first = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
        vecd_regs.RemoveFirst();
        auto second = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
        vecd_regs.RemoveFirst();
        as->ldp(first, second, a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        offset += 16;
      }
    }
  }
  as->mov(a64::sp, arch::fp);
  as->ldp(arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
  as->ret(arch::lr);
#else
  CINDER_UNSUPPORTED
#endif
}

// Emit the function entry sequence (push frame pointer, set up new frame).
void translatePrologue(Environ* env, const Instruction*) {
  arch::Builder* as = env->as;
  asmjit::BaseNode* cursor = as->cursor();
#if defined(CINDER_X86_64)
  as->push(x86::rbp);
  as->mov(x86::rbp, x86::rsp);
#elif defined(CINDER_AARCH64)
  as->stp(arch::fp, arch::lr, a64::ptr_pre(a64::sp, -arch::kFrameRecordSize));
  as->mov(arch::fp, a64::sp);
#else
  CINDER_UNSUPPORTED
#endif
  env->addAnnotation(std::string("Set up frame pointer"), cursor);
}

// Allocate the full stack frame and save callee-saved registers.
// All frame layout values come from Environ, populated after register
// allocation.
void translateSetupFrame(Environ* env, const Instruction*) {
  arch::Builder* as = env->as;

#if defined(CINDER_X86_64)
  // Allocate header + spill space, then push callee-saved registers.
  // Push is 1-2B per register vs 4-7B for movq to a stack slot.
  asmjit::BaseNode* alloc_cursor = as->cursor();
  as->sub(x86::rsp, env->resume_header_and_spill_size);
  env->addAnnotation(std::string("Allocate stack frame"), alloc_cursor);

  asmjit::BaseNode* save_cursor = as->cursor();
  auto saved_regs = env->resume_saved_regs;
  while (!saved_regs.Empty()) {
    as->push(x86::gpq(saved_regs.GetFirst().loc));
    saved_regs.RemoveFirst();
  }
  int arg_buffer_size = env->resume_frame_total_size -
      env->resume_header_and_spill_size -
      env->resume_saved_regs.count() * kPointerSize;
  if (arg_buffer_size > 0) {
    as->sub(x86::rsp, arg_buffer_size);
  }
  env->addAnnotation(std::string("Save callee-saved registers"), save_cursor);
#elif defined(CINDER_AARCH64)
  // allocateHeaderAndSpillSpace()
  asmjit::BaseNode* alloc_cursor = as->cursor();
  arch::sub_immediate(
      as,
      a64::sp,
      a64::sp,
      static_cast<uint64_t>(env->resume_frame_total_size));
  env->addAnnotation(std::string("Allocate stack frame"), alloc_cursor);

  // saveCallerRegisters()
  asmjit::BaseNode* save_cursor = as->cursor();
  auto gp_regs = env->resume_saved_regs & ALL_GP_REGISTERS;
  auto vecd_regs = env->resume_saved_regs & ALL_VECD_REGISTERS;

  arch::sub_immediate(
      as,
      arch::reg_scratch_0,
      arch::fp,
      static_cast<uint64_t>(env->resume_header_and_spill_size));

  int reg_offset = 0;
  if (!gp_regs.Empty()) {
    if (gp_regs.count() % 2 == 1) {
      as->str(
          a64::x(gp_regs.GetFirst().loc),
          a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      gp_regs.RemoveFirst();
      reg_offset += 16;
    }
    while (!gp_regs.Empty()) {
      auto first = a64::x(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
      auto second = a64::x(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
      as->stp(first, second, a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
  if (!vecd_regs.Empty()) {
    if (vecd_regs.count() % 2 == 1) {
      as->str(
          a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
          a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      vecd_regs.RemoveFirst();
      reg_offset += 16;
    }
    while (!vecd_regs.Empty()) {
      auto first = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
      vecd_regs.RemoveFirst();
      auto second = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
      vecd_regs.RemoveFirst();
      as->stp(first, second, a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
  env->addAnnotation(std::string("Save callee-saved registers"), save_cursor);
#else
  CINDER_UNSUPPORTED
#endif
}

// Emit an indirect jump through a memory location. The instruction's single
// input is a MemoryIndirect operand specifying [base + offset].
void translateIndirectJump(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  const OperandBase* input = instr->getInput(0);
  JIT_CHECK(input->isInd(), "IndirectJump input must be memory indirect");

  const auto* mem = input->getMemoryIndirect();
  PhyLocation base = mem->getBaseRegOperand()->getPhyRegister();
  int32_t disp = mem->getOffset();

#if defined(CINDER_X86_64)
  as->jmp(x86::ptr(x86::gpq(base.loc), disp));
#elif defined(CINDER_AARCH64)
  auto ptr = arch::ptr_resolve(as, a64::x(base.loc), disp, arch::reg_scratch_0);
  as->ldr(arch::reg_scratch_br, ptr);
  as->br(arch::reg_scratch_br);
#else
  CINDER_UNSUPPORTED
#endif
}

#if defined(CINDER_AARCH64)

namespace {

using AT = AutoTranslator;

// We do not want to extend AT::getGp to support SP because we only want to
// return SP in very specific circumstances (e.g., building an address relative
// to SP).
arch::Gp getGpOrSP(const OperandBase* operand) {
  if (operand->getPhyRegister() == SP) {
    return a64::sp;
  } else {
    return AT::getGp(operand);
  }
}

// Load the effective address of a scaled index into the given output register
// (used to resolve MemoryIndirect instances).
//
// The multiplier uses x86 SIB log2 encoding: 0 means scale by 1 (2^0),
// 1 means scale by 2 (2^1), 2 means scale by 4 (2^2), 3 means scale by 8
// (2^3).
void leaIndex(
    arch::Builder* as,
    arch::Gp output,
    arch::Gp base,
    arch::Gp index,
    uint8_t multiplier) {
  switch (multiplier) {
    case 0:
      as->add(output, base, index);
      break;
    case 1:
      as->add(output, base, index, a64::lsl(1));
      break;
    case 2:
      as->add(output, base, index, a64::lsl(2));
      break;
    case 3:
      as->add(output, base, index, a64::lsl(3));
      break;
    default:
      JIT_ABORT(
          "Unexpected multiplier {} in leaIndex - should have been lowered "
          "by postgen rewrite",
          multiplier);
  }
}

// Resolve the memory address represented by a MemoryIndirect into the given
// general-purpose register.
void leaIndirect(
    arch::Builder* as,
    arch::Gp output,
    const MemoryIndirect* indirect) {
  auto base = getGpOrSP(indirect->getBaseRegOperand());
  auto indexRegOperand = indirect->getIndexRegOperand();
  auto offset = indirect->getOffset();

  if (indexRegOperand != nullptr) {
    leaIndex(
        as,
        output,
        base,
        AT::getGp(indexRegOperand),
        indirect->getMultipiler());

    base = output;
  }
  arch::add_signed_immediate(as, output, base, offset);
}

// Resolve the memory address represented by a MemoryIndirect into an a64::Mem
// operand suitable for load and store operations.
arch::Mem ptrIndirect(
    arch::Builder* as,
    arch::Gp scratch0,
    arch::Gp scratch1,
    const MemoryIndirect* indirect) {
  auto base = getGpOrSP(indirect->getBaseRegOperand());
  auto indexRegOperand = indirect->getIndexRegOperand();
  auto offset = indirect->getOffset();

  if (indexRegOperand != nullptr) {
    leaIndex(
        as,
        scratch1,
        base,
        AT::getGp(indexRegOperand),
        indirect->getMultipiler());

    base = scratch1;
  }

  return arch::ptr_resolve(as, base, offset, scratch0);
}

void loadToReg(
    arch::Builder* as,
    const OperandBase* output,
    const arch::Mem& input) {
  if (output->isVecD()) {
    as->ldr(AT::getVecD(output), input);
  } else {
    switch (output->dataType()) {
      case OperandBase::k8bit:
        as->ldrb(
            AT::getGp(DataType::k32bit, output->getPhyRegister().loc), input);
        break;
      case OperandBase::k16bit:
        as->ldrh(
            AT::getGp(DataType::k32bit, output->getPhyRegister().loc), input);
        break;
      default:
        as->ldr(AT::getGp(output), input);
        break;
    }
  }
}

void storeFromReg(
    arch::Builder* as,
    const OperandBase* input,
    const arch::Mem& output) {
  if (input->isVecD()) {
    as->str(AT::getVecD(input), output);
  } else {
    switch (input->dataType()) {
      case OperandBase::k8bit:
        as->strb(
            AT::getGp(DataType::k32bit, input->getPhyRegister().loc), output);
        break;
      case OperandBase::k16bit:
        as->strh(
            AT::getGp(DataType::k32bit, input->getPhyRegister().loc), output);
        break;
      default:
        as->str(AT::getGp(input), output);
        break;
    }
  }
}

void translateLea(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = instr->output();
  auto input = instr->getInput(0);

  JIT_CHECK(output->isReg(), "Expected output to be a register");

  if (input->isStack()) {
    arch::add_signed_immediate(
        as, AT::getGp(output), arch::fp, input->getStackSlot().loc);
  } else if (input->isMem()) {
    auto address = reinterpret_cast<uint64_t>(input->getMemoryAddress());
    as->mov(AT::getGp(output), address);
  } else if (input->isInd()) {
    leaIndirect(as, AT::getGp(output), input->getMemoryIndirect());
  } else if (input->isLabel()) {
    asmjit::Label label = input->getDefine()->hasAsmLabel()
        ? input->getDefine()->getAsmLabel()
        : map_get(env->block_label_map, input->getBasicBlock());
    as->adr(AT::getGp(output), label);
  } else {
    JIT_ABORT("Unsupported operand type for Lea: {}", input->type());
  }
}

void translateCall(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = instr->output();
  auto input = instr->getInput(0);

  if (input->isReg()) {
    as->blr(AT::getGp(input));
  } else if (input->isStack()) {
    auto loc = input->getStackSlot().loc;
    as->ldr(
        arch::reg_scratch_br,
        arch::ptr_resolve(as, arch::fp, loc, arch::reg_scratch_0));
    as->blr(arch::reg_scratch_br);
  } else {
    JIT_ABORT("Unsupported operand type for Call: {}", input->type());
  }

  if (instr->origin()) {
    asmjit::Label label = as->newLabel();
    as->bind(label);
    env->pending_debug_locs.emplace_back(label, instr->origin());
  }

  if (output->type() != OperandBase::kNone) {
    if (output->isVecD()) {
      as->mov(AT::getVecD(output), a64::d0);
    } else {
      auto out_reg = AT::getGpOutput(output);
      // Match the source register width to the destination register width.
      // aarch64 mov requires both operands to be the same size.
      if (out_reg.isGpW()) {
        as->mov(out_reg, a64::w0);
      } else {
        as->mov(out_reg, a64::x0);
      }
    }
  }
}

// Our move instruction encapsulates moving a value between registers, setting
// the value of a register, loading a value from memory, and storing a value to
// memory. The operation that will be performed is determined by the
// input/output register combination. In general:
//
// * reg           + reg           = moving
// * reg           + imm           = setting
// * reg           + stack/mem/ind = loading
// * stack/mem/ind + reg/imm       = storing
//
void translateMove(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;
  auto scratch0 = arch::reg_scratch_0;
  auto scratch1 = arch::reg_scratch_1;

  const OperandBase* output = instr->output();
  const OperandBase* input = instr->getInput(0);

  switch (output->type()) {
    case lir::OperandType::kReg:
      switch (input->type()) {
        case lir::OperandType::kReg:
          // Moving a value from a register to a register.
          if (output->isVecD()) {
            if (input->isVecD()) {
              as->fmov(AT::getVecD(output), AT::getVecD(input));
            } else {
              as->fmov(AT::getVecD(output), AT::getGp(input));
            }
          } else {
            if (input->isVecD()) {
              as->fmov(AT::getGp(output), AT::getVecD(input));
            } else {
              as->mov(AT::getGpWiden(output), AT::getGpWiden(input));
            }
          }
          break;
        case lir::OperandType::kStack: {
          // Loading a value from the stack into a register.
          auto ptr = arch::ptr_resolve(
              as, arch::fp, input->getStackSlot().loc, arch::reg_scratch_0);
          if (output->isVecD()) {
            as->ldr(AT::getVecD(output), ptr);
          } else {
            switch (output->dataType()) {
              case OperandBase::k8bit:
                as->ldrb(AT::getGpOutput(output), ptr);
                break;
              case OperandBase::k16bit:
                as->ldrh(AT::getGpOutput(output), ptr);
                break;
              default:
                as->ldr(AT::getGp(output), ptr);
                break;
            }
          }
          break;
        }
        case lir::OperandType::kInd: {
          // Loading a value from an address relative to another register into
          // a register.
          auto ptr = ptrIndirect(
              as,
              arch::reg_scratch_0,
              arch::reg_scratch_1,
              input->getMemoryIndirect());

          loadToReg(as, output, ptr);
          break;
        }
        case lir::OperandType::kImm: {
          // Loading a constant immediate into a register.
          auto constant = input->getConstant();

          if (output->isVecD()) {
            as->fmov(AT::getVecD(output), constant);
          } else if (constant == 0) {
            as->mov(
                AT::getGpWiden(output),
                AT::getGpWiden(output->dataType(), a64::xzr.id()));
          } else {
            as->mov(AT::getGpWiden(output), constant);
          }
          break;
        }
        case lir::OperandType::kNone:
        case lir::OperandType::kVreg:
        case lir::OperandType::kMem:
        case lir::OperandType::kLabel:
          JIT_ABORT(
              "Unsupported operand type for Move: Reg + {}", input->type());
      }
      break;
    case lir::OperandType::kStack: {
      auto ptr = arch::ptr_resolve(
          as, arch::fp, output->getStackSlot().loc, arch::reg_scratch_0);

      if (input->isReg()) {
        // Storing the value of a register to the stack.
        storeFromReg(as, input, ptr);
      } else {
        JIT_ABORT("Unsupported operand type for Move: Stk + {}", input->type());
      }
      break;
    }
    case lir::OperandType::kMem:
      as->mov(scratch0, reinterpret_cast<uint64_t>(output->getMemoryAddress()));

      if (input->isReg()) {
        // Storing the value of a register to an absolute address.
        if (input->isVecD()) {
          as->str(AT::getVecD(input), a64::ptr(scratch0));
        } else {
          as->str(AT::getGpWiden(input), a64::ptr(scratch0));
        }
      } else if (input->isImm()) {
        // Storing a constant immediate to an absolute address.
        as->mov(scratch1, input->getConstant());
        as->str(scratch1, a64::ptr(scratch0));
      } else {
        JIT_ABORT("Unsupported operand type for Move: Mem + {}", input->type());
      }
      break;
    case lir::OperandType::kInd: {
      if (input->isReg()) {
        // Storing the value of a register to an address relative to another
        // register.
        auto ptr =
            ptrIndirect(as, scratch0, scratch1, output->getMemoryIndirect());

        storeFromReg(as, input, ptr);
      } else if (input->isImm()) {
        // Storing a constant immediate to an address relative to another
        // register.
        auto ptr =
            ptrIndirect(as, scratch0, scratch1, output->getMemoryIndirect());

        // Use the output's data type to determine the store width.
        switch (output->dataType()) {
          case OperandBase::k8bit:
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->strb(a64::w(scratch1.id()), ptr);
            break;
          case OperandBase::k16bit:
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->strh(a64::w(scratch1.id()), ptr);
            break;
          case OperandBase::k32bit:
            // Use w register for 4-byte store to avoid overflowing
            // tightly-packed fields.
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->str(a64::w(scratch1.id()), ptr);
            break;
          default:
            as->mov(scratch1, input->getConstant());
            as->str(scratch1, ptr);
            break;
        }
      } else {
        JIT_ABORT("Unsupported operand type for Move: Ind + {}", input->type());
      }
      break;
    }
    case lir::OperandType::kNone:
    case lir::OperandType::kVreg:
    case lir::OperandType::kImm:
    case lir::OperandType::kLabel:
      JIT_ABORT("Unsupported output operand type for Move: {}", output->type());
  }
}

void translateMovConstPool(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;
  auto output = instr->output();
  auto input = instr->getInput(0);
  uint64_t value = static_cast<uint64_t>(input->getConstant());

  // Look up or create constant pool entry for this value.
  asmjit::Label label;
  auto it = env->const_pool_labels.find(value);
  if (it == env->const_pool_labels.end()) {
    label = as->newLabel();
    env->const_pool_labels[value] = label;
  } else {
    label = it->second;
  }

  // Load from constant pool via PC-relative ldr.
  as->ldr(AT::getGpWiden(output), a64::ptr(label));
}

template <
    typename EmitExt8Fn,
    typename EmitExt16Fn,
    typename EmitLoad8Fn,
    typename EmitLoad16Fn>
void translateMovExtOp(
    Environ* env,
    const Instruction* instr,
    const char* opname,
    EmitExt8Fn emit_ext8,
    EmitExt16Fn emit_ext16,
    EmitLoad8Fn emit_load8,
    EmitLoad16Fn emit_load16) {
  a64::Builder* as = env->as;

  auto output = AT::getGpOutput(instr->output());
  const OperandBase* input = instr->getInput(0);
  int input_size = input->sizeInBits();

  if (input->isReg()) {
    auto input_reg = AT::getGp(DataType::k32bit, input->getPhyRegister().loc);

    switch (input_size) {
      case 8:
        emit_ext8(as, output, input_reg);
        break;
      case 16:
        emit_ext16(as, output, input_reg);
        break;
      case 32:
        as->mov(a64::w(output.id()), input_reg);
        break;
      default:
        JIT_ABORT("Unsupported input size for {}: {}", opname, input_size);
    }
  } else if (input->isStack()) {
    auto loc = input->getStackSlot().loc;

    switch (input_size) {
      case 8:
        emit_load8(
            as,
            output,
            arch::ptr_resolve(
                as, arch::fp, loc, arch::reg_scratch_0, arch::AccessSize::k8));
        break;
      case 16:
        emit_load16(
            as,
            output,
            arch::ptr_resolve(
                as, arch::fp, loc, arch::reg_scratch_0, arch::AccessSize::k16));
        break;
      case 32:
        as->ldr(
            a64::w(output.id()),
            arch::ptr_resolve(
                as, arch::fp, loc, arch::reg_scratch_0, arch::AccessSize::k32));
        break;
      default:
        JIT_ABORT("Unsupported input size for {}: {}", opname, input_size);
    }
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, input->type());
  }
}

void translateMovZX(Environ* env, const Instruction* instr) {
  // ARM64 uxtb/uxth/ldrb/ldrh only accept W-register destinations.
  // Writing to W implicitly zeros the upper 32 bits of the X register,
  // so this correctly zero-extends to 64 bits even for k64bit outputs.
  translateMovExtOp(
      env,
      instr,
      "MovZX",
      [](a64::Builder* as, auto output, auto input) {
        as->uxtb(a64::w(output.id()), input);
      },
      [](a64::Builder* as, auto output, auto input) {
        as->uxth(a64::w(output.id()), input);
      },
      [](a64::Builder* as, auto output, auto mem) {
        as->ldrb(a64::w(output.id()), mem);
      },
      [](a64::Builder* as, auto output, auto mem) {
        as->ldrh(a64::w(output.id()), mem);
      });
}

void translateMovSX(Environ* env, const Instruction* instr) {
  translateMovExtOp(
      env,
      instr,
      "MovSX",
      [](a64::Builder* as, auto... args) { as->sxtb(args...); },
      [](a64::Builder* as, auto... args) { as->sxth(args...); },
      [](a64::Builder* as, auto... args) { as->ldrsb(args...); },
      [](a64::Builder* as, auto... args) { as->ldrsh(args...); });
}

void translateMovSXD(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = AT::getGpOutput(instr->output());
  const OperandBase* input = instr->getInput(0);

  if (input->isReg()) {
    auto input_reg = asmjit::a64::w(input->getPhyRegister().loc);
    as->sxtw(output, input_reg);
  } else if (input->isStack()) {
    auto loc = input->getStackSlot().loc;
    auto ptr = arch::ptr_resolve(
        as, arch::fp, loc, arch::reg_scratch_0, arch::AccessSize::k32);
    as->ldrsw(output, ptr);
  } else {
    JIT_ABORT("Unsupported operand type for MovSXD: {}", input->type());
  }
}

void translateUnreachable(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  as->udf(0);
}

void translateNegate(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const OperandBase* opnd0 = instr->getInput(0);

  JIT_CHECK(output->isReg(), "Expected output to be a register");

  auto output_reg = AT::getGpOutput(output);

  if (opnd0->isReg()) {
    as->neg(output_reg, AT::getGpWiden(opnd0));
  } else {
    JIT_ABORT("Unsupported operand type for Negate: {}", opnd0->type());
  }
}

void translateInvert(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const OperandBase* opnd0 = instr->getInput(0);

  JIT_CHECK(output->isReg(), "Expected output to be a register");

  auto output_reg = AT::getGpOutput(output);

  if (opnd0->isReg()) {
    as->mvn(output_reg, AT::getGpWiden(opnd0));
  } else {
    JIT_ABORT("Unsupported operand type for Invert: {}", opnd0->type());
  }
}

template <typename EmitFn>
void translateAddSubOp(
    Environ* env,
    const Instruction* instr,
    const char* opname,
    EmitFn emit) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const OperandBase* opnd0 = instr->getInput(0);
  const OperandBase* opnd1 = instr->getInput(1);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");

  auto output_reg = AT::getGpOutput(output);
  auto opnd0_reg = AT::getGpWiden(opnd0);

  if (opnd1->isImm()) {
    uint64_t constant = opnd1->getConstant();
    JIT_CHECK(arm::Utils::isAddSubImm(constant), "Out of range");

    emit(as, output_reg, opnd0_reg, constant);
  } else if (opnd1->isReg()) {
    emit(as, output_reg, opnd0_reg, AT::getGpWiden(opnd1));
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, opnd1->type());
  }
}

void translateAdd(Environ* env, const Instruction* instr) {
  translateAddSubOp(env, instr, "Add", [](a64::Builder* as, auto... args) {
    as->add(args...);
  });
}

void translateSub(Environ* env, const Instruction* instr) {
  translateAddSubOp(env, instr, "Sub", [](a64::Builder* as, auto... args) {
    as->sub(args...);
  });
}

template <typename EmitFn>
void translateLogicalOp(
    Environ* env,
    const Instruction* instr,
    const char* opname,
    EmitFn emit) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const OperandBase* opnd0 = instr->getInput(0);
  const OperandBase* opnd1 = instr->getInput(1);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");

  auto output_reg = AT::getGpWiden(output);
  auto opnd0_reg = AT::getGpWiden(opnd0);

  if (opnd1->isImm()) {
    uint64_t constant = opnd1->getConstant();
    uint32_t width = output->sizeInBits() <= 32 ? 32 : 64;
    JIT_CHECK(arm::Utils::isLogicalImm(constant, width), "Invalid constant");

    emit(as, output_reg, opnd0_reg, constant);
  } else if (opnd1->isReg()) {
    emit(as, output_reg, opnd0_reg, AT::getGpWiden(opnd1));
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, opnd1->type());
  }
}

void translateAnd(Environ* env, const Instruction* instr) {
  translateLogicalOp(env, instr, "And", [](a64::Builder* as, auto... args) {
    as->and_(args...);
  });
}

void translateOr(Environ* env, const Instruction* instr) {
  translateLogicalOp(env, instr, "Or", [](a64::Builder* as, auto... args) {
    as->orr(args...);
  });
}

void translateXor(Environ* env, const Instruction* instr) {
  translateLogicalOp(env, instr, "Xor", [](a64::Builder* as, auto... args) {
    as->eor(args...);
  });
}

void translateMul(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const OperandBase* opnd0 = instr->getInput(0);
  const OperandBase* opnd1 = instr->getInput(1);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");

  auto output_reg = AT::getGpWiden(output);
  auto opnd0_reg = AT::getGpWiden(opnd0);

  if (opnd1->isReg()) {
    as->mul(output_reg, opnd0_reg, AT::getGpWiden(opnd1));
  } else {
    JIT_ABORT("Unsupported operand type for Mul: {}", opnd1->type());
  }
}

void translateMulAdd(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = instr->output();
  auto opnd0 = instr->getInput(0);
  auto opnd1 = instr->getInput(1);
  auto opnd2 = instr->getInput(2);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");
  JIT_CHECK(opnd1->isReg(), "Expected opnd1 to be a register");
  JIT_CHECK(opnd2->isReg(), "Expected opnd2 to be a register");

  // madd Rd, Rn, Rm, Ra  =>  Rd = Ra + Rn * Rm
  as->madd(
      AT::getGp(output), AT::getGp(opnd0), AT::getGp(opnd1), AT::getGp(opnd2));
}

template <typename EmitFn>
void translateDivOp(
    Environ* env,
    const Instruction* instr,
    const char* opname,
    EmitFn emit) {
  a64::Builder* as = env->as;

  const OperandBase* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);

  // Division instructions may have an extra leading Imm{0} input (used by x86
  // for the high half of the dividend). Skip it on AArch64.
  size_t base = 0;
  if (instr->getNumInputs() == 3 && instr->getInput(0)->isImm()) {
    base = 1;
  }
  const OperandBase* opnd0 = instr->getInput(base);
  const OperandBase* opnd1 = instr->getInput(base + 1);

  JIT_CHECK(output->isReg(), "Expected output to be a register");
  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");

  // Use getGpOutput to get the correct register width. sdiv/udiv require all
  // operands to be the same width. getGpOutput returns w(reg) for k32bit and
  // x(reg) for k64bit, matching the hardware instruction requirements.
  // (getGpWiden would return x(reg) for k32bit, causing sdiv to interpret
  // zero-extended 32-bit values as 64-bit, giving wrong results for negatives.)
  auto output_reg = AT::getGpOutput(output);
  auto opnd0_reg = AT::getGpOutput(opnd0);

  if (opnd1->isReg()) {
    emit(as, output_reg, opnd0_reg, AT::getGpOutput(opnd1));
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, opnd1->type());
  }
}

void translateDiv(Environ* env, const Instruction* instr) {
  translateDivOp(env, instr, "Div", [](a64::Builder* as, auto... args) {
    as->sdiv(args...);
  });
}

void translateDivUn(Environ* env, const Instruction* instr) {
  translateDivOp(env, instr, "DivUn", [](a64::Builder* as, auto... args) {
    as->udiv(args...);
  });
}

void translatePush(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* operand = instr->getInput(0);

  if (operand->isReg()) {
    auto reg = AT::getGpWiden(operand);
    as->str(reg, a64::ptr_pre(a64::sp, -16));
  } else if (operand->isStack()) {
    auto loc = operand->getStackSlot().loc;
    auto ptr = arch::ptr_resolve(as, arch::fp, loc, arch::reg_scratch_1);
    as->ldr(arch::reg_scratch_0, ptr);
    as->str(arch::reg_scratch_0, a64::ptr_pre(a64::sp, -16));
  } else {
    JIT_ABORT("Unsupported operand type for push: {}", operand->type());
  }
}

void translatePop(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* operand = instr->output();

  if (operand->isReg()) {
    auto reg = AT::getGpWiden(operand);
    as->ldr(reg, a64::ptr_post(a64::sp, 16));
  } else if (operand->isStack()) {
    auto loc = operand->getStackSlot().loc;
    auto ptr = arch::ptr_resolve(as, arch::fp, loc, arch::reg_scratch_1);
    as->ldr(arch::reg_scratch_0, a64::ptr_post(a64::sp, 16));
    as->str(arch::reg_scratch_0, ptr);
  } else {
    JIT_ABORT("Unsupported operand type for pop: {}", operand->type());
  }
}

void translateExchange(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* opnd0 = instr->output();
  const OperandBase* opnd1 = instr->getInput(0);

  JIT_CHECK(opnd0->isReg(), "Expected opnd0 to be a register");
  JIT_CHECK(opnd1->isReg(), "Expected opnd1 to be a register");

  if (opnd0->isVecD() && opnd1->isVecD()) {
    auto vec0 = AT::getVecD(opnd0);
    auto vec1 = AT::getVecD(opnd1);

    as->eor(vec0, vec0, vec1);
    as->eor(vec1, vec1, vec0);
    as->eor(vec0, vec0, vec1);
  } else {
    auto reg0 = AT::getGpWiden(opnd0);
    auto reg1 = AT::getGpWiden(opnd1);
    auto scratch = AT::getGpWiden(opnd0->dataType(), arch::reg_scratch_0.id());

    as->mov(scratch, reg0);
    as->mov(reg0, reg1);
    as->mov(reg1, scratch);
  }
}

void translateCmp(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const OperandBase* inp0 = instr->getInput(0);
  const OperandBase* inp1 = instr->getInput(1);

  JIT_CHECK(inp0->isReg(), "Expected first input to be a register");

  if (inp1->isReg()) {
    if (inp0->isVecD() && inp1->isVecD()) {
      as->fcmp(AT::getVecD(inp0), AT::getVecD(inp1));
    } else {
      as->cmp(AT::getGpWiden(inp0), AT::getGpWiden(inp1));
    }
  } else if (inp1->isImm()) {
    auto constant = inp1->getConstant();
    arch::cmp_immediate(as, AT::getGpWiden(inp0), constant);
  } else {
    JIT_ABORT(
        "Unsupported operand types for cmp: {} {}", inp0->type(), inp1->type());
  }
}

template <typename EmitFn>
void translateIncDecOp(
    Environ* env,
    const Instruction* instr,
    const char* opname,
    EmitFn emit) {
  a64::Builder* as = env->as;

  auto opnd = instr->getInput(0);

  if (opnd->isReg()) {
    // We have to do adds/subs here, because implicitly our LIR relies on the
    // Inc/Dec instructions setting flags.
    emit(as, AT::getGpWiden(opnd), AT::getGpWiden(opnd), 1);
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, opnd->dataType());
  }
}

void translateInc(Environ* env, const Instruction* instr) {
  translateIncDecOp(env, instr, "Inc", [](a64::Builder* as, auto... args) {
    as->adds(args...);
  });
}

void translateDec(Environ* env, const Instruction* instr) {
  translateIncDecOp(env, instr, "Dec", [](a64::Builder* as, auto... args) {
    as->subs(args...);
  });
}

void translateBitTest(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto test_reg = AT::getGpWiden(instr->getInput(0));
  auto bit_pos = instr->getInput(1)->getConstant();

  uint64_t mask = 1ULL << bit_pos;
  JIT_CHECK(
      arm::Utils::isLogicalImm(mask, 64),
      "All single bits should be able to be tested");

  as->tst(test_reg, mask);
}

void translateTst(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto opnd0 = instr->getInput(0);
  auto opnd1 = instr->getInput(1);
  auto data_type = opnd0->dataType();

  // For 8-bit and 16-bit values, shift the valid bits into the high bits of a
  // 32-bit register using LSL so that TST sets the N and Z flags correctly for
  // the sub-register width.
  int shift = 0;
  if (data_type == jit::lir::OperandBase::k8bit) {
    shift = 24;
  } else if (data_type == jit::lir::OperandBase::k16bit) {
    shift = 16;
  }

  if (shift) {
    auto w0 = asmjit::a64::w(opnd0->getPhyRegister().loc);
    auto w1 = asmjit::a64::w(opnd1->getPhyRegister().loc);
    auto scratch = arch::reg_scratch_0.w();
    as->lsl(scratch, w0, shift);
    as->tst(scratch, w1, arm::Shift(arm::ShiftOp::kLSL, shift));
  } else {
    as->tst(AT::getGp(opnd0), AT::getGp(opnd1));
  }
}

void translateSelect(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = AT::getGpOutput(instr->output());
  auto condition_op = instr->getInput(0);
  arch::Gp condition_reg;
  switch (condition_op->dataType()) {
    case jit::lir::OperandBase::k8bit:
    case jit::lir::OperandBase::k16bit:
      condition_reg =
          AT::getGp(DataType::k32bit, condition_op->getPhyRegister().loc);
      as->and_(
          condition_reg,
          condition_reg,
          (1 << bitSize(condition_op->dataType())) - 1);
      break;
    default:
      condition_reg = AT::getGp(condition_op);
      break;
  }
  auto true_val_reg = AT::getGpWiden(instr->getInput(1));
  auto false_val_reg = AT::getGpWiden(instr->getInput(2));

  as->cmp(condition_reg, 0);
  as->csel(output, true_val_reg, false_val_reg, a64::CondCode::kNE);
}

} // namespace

#endif

} // namespace jit::codegen::autogen
