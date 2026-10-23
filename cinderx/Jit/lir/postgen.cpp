// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/postgen.h"

#include "cinderx/Common/define.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/lir/printer.h"

#include <unordered_map>

namespace cinderx::jit::rt {
void batchDecref(cinderx::jit::TaggedPyObject* args, int nargs);
} // namespace cinderx::jit::rt

using namespace cinderx::jit::codegen;

namespace cinderx::jit::lir {

namespace {

// Constant fold unary operations with immediate inputs.
// Negate(Imm(c)) → Move(Imm(-c))
// Invert(Imm(c)) → Move(Imm(~c))
// IntToBool(Imm(c)) → Move(Imm(c ? 1 : 0))
RewriteResult rewriteConstantFoldUnaryOps(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  switch (instr->opcode()) {
    case Opcode::kIntToBool:
    case Opcode::kInvert:
    case Opcode::kNegate:
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
    case Opcode::kNegate:
      result = static_cast<uint64_t>(-static_cast<int64_t>(constant));
      break;
    case Opcode::kInvert:
      result = ~constant;
      break;
    case Opcode::kIntToBool:
      result = constant ? 1 : 0;
      break;
    default:
      return kUnchanged;
  }

  instr->setOpcode(Opcode::kMove);
  auto dt = input->dataType();
  input->setConstant(result, dt);
  return kChanged;
}

// Fix constant input position. If a binary operation has a constant input,
// always put it as the second operand (or move the 2nd to a register for div
// instructions)
RewriteResult rewriteBinaryOpConstantPosition(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto block = instr->basicBlock();

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
          Opcode::kMove,
          OutVReg{constant_size},
          Imm{constant, constant_size});

      instr->setInput(idx, std::make_unique<Operand>(move, Operand::kLinked));
      changed = true;
    }
    return changed ? kChanged : kUnchanged;
  }

  bool is_commutative_or_compare = instr->isAdd() || instr->isAnd() ||
      instr->isMul() || instr->isOr() || instr->isXor() ||
      isCompare(instr->opcode());
  bool is_shift = instr->isLShift() || instr->isRShift() || instr->isRShiftUn();
  if (!is_commutative_or_compare && !is_shift && !instr->isSub()) {
    return kUnchanged;
  }

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
      instr->setCondition(swapOperands(instr->condition()));
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
      Opcode::kMove,
      OutVReg{constant_size},
      Imm{constant, constant_size});
  instr->setInput(0, std::make_unique<Operand>(move, Operand::kLinked));

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
      !isCompare(instr->opcode())) {
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
  if (instr->isAdd() || instr->isSub() || isCompare(instr->opcode())) {
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

  auto block = instr->basicBlock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Opcode::kMove,
      OutVReg{in1->dataType()},
      Imm{constant, in1->dataType()});

  // If the first operand is smaller in size than the second operand, replace
  // the first operand with a sign-extended version that matches the size of the
  // second operand.
  if (instr->getInput(0)->sizeInBits() < in1->sizeInBits()) {
    auto sext = block->allocateInstrBefore(
        instr_iter, Opcode::kSext, OutVReg{in1->dataType()});
    sext->appendInput(instr->releaseInput(0));
    instr->setInput(0, std::make_unique<Operand>(sext, Operand::kLinked));
  }

  // Replace the constant with the move.
  instr->setInput(1, std::make_unique<Operand>(move, Operand::kLinked));

  return kChanged;
}

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

  auto block = instr->basicBlock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Opcode::kMove,
      OutVReg(),
      Imm(target_imm, target_opnd->dataType()));
  instr->setInput(
      kTargetIndex, std::make_unique<Operand>(move, Operand::kLinked));
  return kChanged;
}

// Rewrite LoadArg to Bind and allocate a physical register for its input.
RewriteResult rewriteLoadArg(instr_iter_t instr_iter, Environ* env) {
  auto instr = instr_iter->get();
  if (!instr->isLoadArg()) {
    return kUnchanged;
  }
  instr->setOpcode(Opcode::kBind);
  JIT_CHECK(instr->getNumInputs() == 1, "expected one input");
  Operand* input = instr->getInput(0);
  JIT_CHECK(input->isImm(), "expected constant arg index as input");
  size_t arg_idx = static_cast<size_t>(input->getConstant());
  JIT_CHECK(
      arg_idx < env->arg_locations.size(),
      "arg index {} out of range for {} arg locations",
      arg_idx,
      env->arg_locations.size());
  JIT_CHECK(
      arg_idx < env->arg_locations.size(),
      "arg index {} out of range for {} arg locations",
      arg_idx,
      env->arg_locations.size());
  auto loc = env->arg_locations[arg_idx];
  input->setPhyRegOrStackSlot(loc);
  input->setDataType(instr->output()->dataType());
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
[[maybe_unused]] RewriteResult rewriteGuardHasType(instr_iter_t instr_iter) {
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
  Instruction* guard_var_def = guard_var->getLinkedInstr();

  auto block = instr->basicBlock();
  constexpr int32_t kObTypeOffset = offsetof(PyObject, ob_type);

  // type_vreg = Load([guard_var + ob_type_offset])
  auto type_load = block->allocateInstrBefore(
      instr_iter, Opcode::kLoad, OutVReg{DataType::k64bit});
  type_load->allocateMemoryIndirectInput(guard_var_def, kObTypeOffset);

  // Replace guard var with the loaded type and change kind to kIs.
  instr->setInput(
      kGuardVarIndex, std::make_unique<Operand>(type_load, Operand::kLinked));
  instr->getInput(kKindIndex)->setConstant(kIs);

  return kChanged;
}

// On AArch64, lower Load with an absolute memory address (kMem) input into
// Move(Imm{addr} → vreg) + Load(Ind{vreg, 0} → output). ARM64 cannot encode a
// 64-bit absolute address inline, so translateLoad used a scratch register.
// This rewrite lets register allocation handle it instead.
[[maybe_unused]] RewriteResult rewriteLoadAbsoluteAddress(
    instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isLoad()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (!input->isMem()) {
    return kUnchanged;
  }

  auto block = instr->basicBlock();
  auto addr = reinterpret_cast<uint64_t>(input->getMemoryAddress());

  // addr_vreg = Move(Imm{addr})
  auto addr_move = block->allocateInstrBefore(
      instr_iter,
      Opcode::kMove,
      OutVReg{DataType::kObject},
      Imm{addr, DataType::kObject});

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
//   - Load "Rm": IS the canonical load (the lowering target)
//   - Zext/Sext: specialized sign/zero-extending loads from stack
//   - Lea: takes the ADDRESS of a stack slot, not the value
//   - Call: late-created by PostRegAllocRewrite via setOpcode()
//   - Negate/Invert: handled by AArch64 target selection
//   - Inc/Dec: handled by AArch64 target selection
//   - Select: handled by AArch64 target selection
//   - EpilogueEnd: special return-value handling
//   - Pop: stack output, not input
bool lowerStackInputToVreg(instr_iter_t instr_iter, size_t idx) {
  auto instr = instr_iter->get();
  auto input = instr->getInput(idx);
  if (!input->isStack()) {
    return false;
  }

  auto loc = input->getStackSlot();
  auto dt = input->dataType();
  auto move = instr->basicBlock()->allocateInstrBefore(
      instr_iter, Opcode::kLoad, OutVReg{dt}, Stk{loc, dt});
  instr->setInput(idx, std::make_unique<Operand>(move, Operand::kLinked));
  return true;
}

RewriteResult rewriteAllStackInputsToVreg(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  bool changed = false;
  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    changed |= lowerStackInputToVreg(instr_iter, i);
  }
  return changed ? kChanged : kUnchanged;
}

RewriteResult rewriteSingleStackInputToVreg(
    instr_iter_t instr_iter,
    size_t idx) {
  return lowerStackInputToVreg(instr_iter, idx) ? kChanged : kUnchanged;
}

[[maybe_unused]] RewriteResult rewriteStackInputToVreg(
    instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (instr->isAdd() || instr->isSub() || instr->isXor() || instr->isAnd() ||
      instr->isOr() || instr->isMul() || isCompare(instr->opcode())) {
    return rewriteAllStackInputsToVreg(instr_iter);
  }

  if (instr->isDiv() || instr->isDivUn()) {
    return rewriteAllStackInputsToVreg(instr_iter);
  }

  switch (instr->opcode()) {
    case Opcode::kPush:
      return rewriteSingleStackInputToVreg(instr_iter, 0);
    default:
      return kUnchanged;
  }
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
//   - Select: handled by AArch64 target selection
//   - BranchBitSet/BranchBitNotSet input 1: isLogicalImm(1<<n) always encodes
//   - Move "Ri" (load immediate to register): this IS the lowering target
//   - Inc/Dec: hardcoded constant 1, no immediate operand
bool lowerImmediateInputToVreg(instr_iter_t instr_iter, size_t idx) {
  auto instr = instr_iter->get();
  auto input = instr->getInput(idx);
  if (!input->isImm()) {
    return false;
  }

  auto move = instr->basicBlock()->allocateInstrBefore(
      instr_iter,
      Opcode::kMove,
      OutVReg{input->dataType()},
      Imm{input->getConstant(), input->dataType()});
  instr->setInput(idx, std::make_unique<Operand>(move, Operand::kLinked));
  return true;
}

RewriteResult rewritePushImmediateToVreg(instr_iter_t instr_iter) {
  // ARM str can't take an immediate data operand.
  return lowerImmediateInputToVreg(instr_iter, 0) ? kChanged : kUnchanged;
}

RewriteResult rewriteStoreImmediateToVreg(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  auto output = instr->output();
  if (!output->isInd() && !output->isStack()) {
    return kUnchanged;
  }
  // AArch64 can store the zero register (WZR/XZR) directly to indirect memory
  // without needing a temporary register.
  if (output->isInd()) {
    auto input = instr->getInput(0);
    if (input->isImm() && input->getConstant() == 0) {
      return kUnchanged;
    }
  }
  return lowerImmediateInputToVreg(instr_iter, 0) ? kChanged : kUnchanged;
}

[[maybe_unused]] RewriteResult rewriteNonBinaryImmediateToVreg(
    instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  switch (instr->opcode()) {
    case Opcode::kPush:
      return rewritePushImmediateToVreg(instr_iter);
    case Opcode::kStore:
      return rewriteStoreImmediateToVreg(instr_iter);
    default:
      return kUnchanged;
  }
}

// For call instructions with non-register inputs
// (Imm or Stack), insert a Move to load the call target into a vreg so
// translateCall only needs blr(reg).
[[maybe_unused]] RewriteResult rewriteCallInput(instr_iter_t instr_iter) {
  auto instr = instr_iter->get();
  if (!instr->isCall() && !instr->isCVarArgCall() && !instr->isVarArgCall() &&
      !instr->isVectorCall() && !instr->isVectorCallTstate()) {
    return kUnchanged;
  }

  auto input = instr->getInput(0);
  if (input->isReg() || input->isLinked()) {
    return kUnchanged;
  }

  auto block = instr->basicBlock();

  if (input->isImm()) {
    // On aarch64, leave immediate call targets as-is so translateCall can emit
    // bl(imm) which uses asmjit's relaxation to pick the optimal encoding
    // (direct bl or ldr+blr via address table).
    if constexpr (kBuildArch == Arch::kAarch64) {
      return kUnchanged;
    }
    auto move = block->allocateInstrBefore(
        instr_iter,
        Opcode::kMove,
        OutVReg{DataType::k64bit},
        Imm{input->getConstant(), DataType::k64bit});
    instr->setInput(0, std::make_unique<Operand>(move, Operand::kLinked));
    return kChanged;
  }

  if (input->isStack()) {
    auto loc = input->getStackSlot();
    auto dt = input->dataType();
    auto move = block->allocateInstrBefore(
        instr_iter, Opcode::kLoad, OutVReg{dt}, Stk{loc, dt});
    instr->setInput(0, std::make_unique<Operand>(move, Operand::kLinked));
    return kChanged;
  }

  return kUnchanged;
}

// Returns true if the given 64-bit value needs more than 2 ARM64 mov
// instructions (movz/movk or movn/movk) to load.
bool needsMoreThanTwoMovInstructions(uint64_t value) {
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
  // movz approach needs `non_zero` instructions.
  // movn approach needs `4 - all_ones` instructions.
  return std::min(non_zero, 4 - all_ones) > 2;
}

// For Move instructions loading large 64-bit immediates (needing 3-4
// movz/movk), if the same value appears more than once in the function,
// rewrite to MovConstPool to load from a constant pool via a single ldr.
[[maybe_unused]] RewriteResult rewriteMovConstPool(
    function_rewrite_arg_t func) {
  // Phase 1: Count occurrences of large immediate values.
  std::unordered_map<uint64_t, int> value_counts;
  for (auto& block : func->basicBlocks()) {
    for (auto it = block->instructions().begin();
         it != block->instructions().end();
         ++it) {
      auto& instr = *it;
      if (!instr->isMove()) {
        continue;
      }
      if (instr->getNumInputs() != 1) {
        continue;
      }
      auto* input = instr->getInput(0);
      if (!input->isImm()) {
        continue;
      }
      auto* output = instr->output();
      if (bitSize(output->dataType()) != 64) {
        continue;
      }
      uint64_t val = static_cast<uint64_t>(input->getConstant());
      if (needsMoreThanTwoMovInstructions(val)) {
        value_counts[val]++;
      }
    }
  }

  // Phase 2: Rewrite duplicates to MovConstPool.
  bool changed = false;
  for (auto& block : func->basicBlocks()) {
    for (auto it = block->instructions().begin();
         it != block->instructions().end();
         ++it) {
      auto& instr = *it;
      if (!instr->isMove()) {
        continue;
      }
      if (instr->getNumInputs() != 1) {
        continue;
      }
      auto* input = instr->getInput(0);
      if (!input->isImm()) {
        continue;
      }
      uint64_t val = static_cast<uint64_t>(input->getConstant());
      auto count_it = value_counts.find(val);
      if (count_it != value_counts.end() && count_it->second > 1) {
        auto* output = instr->output();
        if (bitSize(output->dataType()) != 64) {
          continue;
        }
        if (output->isInd() || output->isStack()) {
          // Memory destination: split into a MovConstPool load into a VReg
          // followed by a Move from that VReg to the memory destination.
          // This avoids scratch register clobbering in autogen.
          auto pool_load = block->allocateInstrBefore(
              it,
              Opcode::kMovConstPool,
              OutVReg{input->dataType()},
              Imm{input->getConstant(), input->dataType()});
          instr->setInput(
              0, std::make_unique<Operand>(pool_load, Operand::kLinked));
        } else {
          instr->setOpcode(Opcode::kMovConstPool);
        }
        changed = true;
      }
    }
  }

  return changed ? kChanged : kUnchanged;
}

bool hasHelperTarget(const Instruction& instr, uint64_t helper) {
  if (!instr.isCall() && !instr.isCVarArgCall() && !instr.isVarArgCall() &&
      !instr.isVectorCall() && !instr.isVectorCallTstate()) {
    return false;
  }
  if (instr.getNumInputs() == 0) {
    return false;
  }

  const Operand* target = instr.getInput(0);
  if (target->isImm()) {
    return target->getConstant() == helper;
  }
  if (!target->isLinked()) {
    return false;
  }

  const Instruction* target_instr = target->getLinkedInstr();
  if (!target_instr->isMove() || target_instr->getNumInputs() != 1) {
    return false;
  }

  const Operand* move_input = target_instr->getInput(0);
  return move_input->isImm() && move_input->getConstant() == helper;
}

bool shouldPreserveTaggedCallArgs(const Instruction& instr) {
  return instr.isVarArgCall() &&
      hasHelperTarget(instr, reinterpret_cast<uint64_t>(rt::batchDecref));
}

// Strip deferred-RC tag bits from all kObject values before they
// are used in memory dereferences, passed to C functions, compared, used
// in guards, or returned. Since PyObject* is aligned enough for CPython's
// stack-ref tag bits, this is a no-op for non-tagged pointers. Non-PyObject*
// pointers must use k64bit to avoid incorrect stripping.
[[maybe_unused]] RewriteResult rewriteStripObjectPointers(
    function_rewrite_arg_t func) {
  bool changed = false;
  for (auto& block : func->basicBlocks()) {
    std::unordered_map<Instruction*, Instruction*> strip_cache;

    for (auto it = block->instructions().begin();
         it != block->instructions().end();
         ++it) {
      auto* instr = it->get();

      auto getOrCreateStrip = [&](Instruction* base) -> Instruction* {
        auto found = strip_cache.find(base);
        if (found != strip_cache.end()) {
          return found->second;
        }
        // Call operands must be passed to C without deferred-RC tag bits.  If
        // the object value is a tagged immediate, strip it while materializing
        // the immediate and avoid creating a virtual-register dependency.
        if (base->isMove() && base->getNumInputs() == 1 &&
            base->getInput(0)->isImm()) {
          uint64_t value = base->getInput(0)->getConstant() &
              ~static_cast<uint64_t>(kPyObjectTagBits);
          Instruction* strip = block->allocateInstrBefore(
              it,
              Opcode::kMove,
              OutVReg{DataType::kObjectUntagged},
              Imm{value, DataType::k64bit});
          strip_cache[base] = strip;
          changed = true;
          return strip;
        }
        // Do not build the strip directly from `base`.  `base` can be a
        // long-lived Python object that remains live for deopt metadata or
        // later decref/incref work across the same callsite.  Around calls,
        // register allocation may split and spill that long-lived interval
        // while also reusing its physical register for call setup.  A strip
        // inserted here is a short-lived derived call operand, not another use
        // of the long-lived tagged object.  Materialize an adjacent copy first
        // so regalloc sees a local copy-then-strip def-use and cannot rewrite
        // the strip to read a stale reused register.
        Instruction* copy = block->allocateInstrBefore(
            it, Opcode::kMove, OutVReg{base->output()->dataType()}, VReg{base});
        Instruction* strip = block->allocateInstrBefore(
            it,
            Opcode::kAnd,
            OutVReg{DataType::kObjectUntagged},
            VReg{copy},
            Imm{~static_cast<uint64_t>(kPyObjectTagBits), DataType::k64bit});
        strip_cache[base] = strip;
        changed = true;
        return strip;
      };

      // Strip kObject Ind bases.
      for (size_t i = 0; i < instr->getNumInputs(); ++i) {
        Operand* operand = instr->getInput(i);
        if (!operand->isInd()) {
          continue;
        }
        MemoryIndirect* indirect = operand->getMemoryIndirect();
        Operand* base = indirect->getBaseRegOperand();
        if (base == nullptr || !base->isLinked() ||
            base->dataType() != DataType::kObject) {
          continue;
        }
        indirect->resetMemoryIndirectBase(
            getOrCreateStrip(base->getLinkedInstr()));
      }

      // Strip kObject OutInd bases.
      {
        auto* out = instr->output();
        if (out->isInd()) {
          MemoryIndirect* indirect = out->getMemoryIndirect();
          Operand* base = indirect->getBaseRegOperand();
          if (base != nullptr && base->isLinked() &&
              base->dataType() == DataType::kObject) {
            indirect->resetMemoryIndirectBase(
                getOrCreateStrip(base->getLinkedInstr()));
          }
        }
      }

      // Assert that guards were already stripped by the LIR generator.
      if (instr->isGuard()) {
        auto kind =
            static_cast<InstrGuardKind>(instr->getInput(0)->getConstant());
        if (kind == InstrGuardKind::kIs || kind == InstrGuardKind::kHasType) {
          JIT_DCHECK(
              instr->getInput(2)->dataType() != DataType::kObject,
              "Guard kIs/kHasType input must be stripped to kObjectUntagged "
              "before reaching the postgen strip pass: {}",
              *instr);
        }
      }

      // Strip kObject call args, compare operands, and return values.
      // rt::batchDecref needs tagged deferred-RC refs so the helper can
      // recognize and skip them; every other call should still see untagged
      // object pointers.
      bool is_call = instr->isCall() || instr->isCVarArgCall() ||
          instr->isVectorCall() || instr->isVectorCallTstate() ||
          instr->isVarArgCall();
      bool strip_call_args = is_call && !shouldPreserveTaggedCallArgs(*instr);
      bool is_compare = instr->isCompare() &&
          (instr->condition() == Condition::kEqual ||
           instr->condition() == Condition::kNotEqual);
      bool is_epilogue = instr->isEpilogueEnd();
      if (!strip_call_args && !is_compare && !is_epilogue) {
        continue;
      }
      size_t start = 0;
      if (strip_call_args) {
        // VectorCallTstate has fixed inputs for helper, flags, and tstate
        // before the callable and Python object arguments.  VectorCall has the
        // same shape minus the tstate.
        if (instr->isVectorCallTstate()) {
          start = 3;
        } else if (instr->isVectorCall()) {
          start = 2;
        } else {
          start = 1;
        }
      }
      size_t end = instr->getNumInputs();
      for (size_t i = start; i < end; ++i) {
        Operand* operand = instr->getInput(i);
        if (!operand->isLinked() || operand->dataType() != DataType::kObject) {
          continue;
        }
        Instruction* strip = getOrCreateStrip(operand->getLinkedInstr());
        instr->setInput(i, std::make_unique<Operand>(strip, Operand::kLinked));
      }
    }
  }
  return changed ? kChanged : kUnchanged;
}

} // namespace

void PostGenerationRewrite::registerRewrites() {
  registerOneRewriteFunction(rewriteConstantFoldUnaryOps, 1);
  registerOneRewriteFunction(rewriteBinaryOpConstantPosition, 1);
  registerOneRewriteFunction(rewriteBinaryOpLargeConstant, 1);
  registerOneRewriteFunction(rewriteGuardLargeConstant, 1);
  registerOneRewriteFunction(rewriteLoadArg, 1);

  if constexpr (kBuildArch == Arch::kAarch64) {
    registerOneRewriteFunction(rewriteGuardHasType, 1);
    registerOneRewriteFunction(rewriteLoadAbsoluteAddress, 1);
    registerOneRewriteFunction(rewriteStackInputToVreg, 1);
    registerOneRewriteFunction(rewriteNonBinaryImmediateToVreg, 1);
    registerOneRewriteFunction(rewriteCallInput, 1);
    registerOneRewriteFunction(rewriteMovConstPool, 1);
  }

  if constexpr (kFreeThreadedBuild) {
    registerOneRewriteFunction(rewriteStripObjectPointers, 2);
  }
}

} // namespace cinderx::jit::lir
