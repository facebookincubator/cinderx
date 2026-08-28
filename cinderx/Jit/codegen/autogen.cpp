// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/autogen.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/gen_asm_utils.h"
#include "cinderx/Jit/codegen/tsan.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/printer.h"
#include "cinderx/module_state.h"

using namespace asmjit;
using namespace cinderx::jit::lir;
using namespace cinderx::jit::codegen;

namespace cinderx::jit::codegen::autogen {

namespace {

#if defined(CINDER_X86_64)
using AsmCondCode = x86::CondCode;
#elif defined(CINDER_AARCH64)
using AsmCondCode = arm::CondCode;
#endif

#if defined(CINDER_X86_64) || defined(CINDER_AARCH64)
// LIR spells its conditions the same way asmjit does, on both targets.
AsmCondCode asmCondCode(lir::Condition cond) {
  switch (cond) {
#define TO_ASMJIT(NAME, ...)    \
  case lir::Condition::k##NAME: \
    return AsmCondCode::k##NAME;
    FOREACH_LIR_CONDITION(TO_ASMJIT)
#undef TO_ASMJIT
    case lir::Condition::kInvalid:
      break;
  }
  JIT_THROW("Cannot encode invalid condition code {}", static_cast<int>(cond));
}

void emitBranchCC(
    arch::Builder* as,
    lir::Condition cond,
    const asmjit::Label& label) {
#if defined(CINDER_X86_64)
  as->j(asmCondCode(cond), label);
#else
  as->b(asmCondCode(cond), label);
#endif
}
#endif

bool isMemoryMoveOperand(const lir::Operand* operand) {
  return operand->isStack() || operand->isMem() || operand->isInd();
}

void checkMoveRelaxedOperandShape(const Instruction* instr) {
  JIT_DCHECK(
      instr->isMoveRelaxed(), "Expected kMoveRelaxed, got {}", instr->opname());

  auto* output = instr->output();
  auto* input = instr->getInput(0);

  bool is_valid_load = output->isReg() && isMemoryMoveOperand(input);
  bool is_valid_store =
      isMemoryMoveOperand(output) && (input->isReg() || input->isImm());

  JIT_CHECK(
      is_valid_load || is_valid_store,
      "kMoveRelaxed only supports memory->register loads and "
      "register/immediate->memory stores, got {} <- {}",
      output->type(),
      input->type());
}

} // namespace

arch::Mem AsmIndirectOperandBuilder(const lir::Operand* operand) {
  JIT_DCHECK(operand->isInd(), "operand should be an indirect reference");

#if defined(CINDER_X86_64)
  auto indirect = operand->getMemoryIndirect();

  lir::Operand* base = indirect->getBaseRegOperand();
  lir::Operand* index = indirect->getIndexRegOperand();

  if (index == nullptr) {
    return asmjit::x86::ptr(
        x86::gpq(base->getPhyRegister().loc), indirect->getOffset());
  } else {
    return asmjit::x86::ptr(
        x86::gpq(base->getPhyRegister().loc),
        x86::gpq(index->getPhyRegister().loc),
        indirect->getMultiplier(),
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
int getOperandSize(const Instruction* instr, const lir::Operand* operand) {
  switch (operandSizeType(instr->opcode())) {
    case OperandSizeType::kAlways64:
      return 64;
    case OperandSizeType::kOut: {
      // Match LIROperandMapper<0> behavior: use the output if present,
      // otherwise use input 0 (which post-alloc rewrites may have resized).
      if (instr->getNumOutputs() > 0) {
        return static_cast<int>(instr->output()->sizeInBits());
      }
      return static_cast<int>(instr->getInput(0)->sizeInBits());
    }
    case OperandSizeType::kDefault:
    default:
      return static_cast<int>(operand->sizeInBits());
  }
}

int getOperandSizeInBytes(
    const Instruction* instr,
    const lir::Operand* operand) {
  return getOperandSize(instr, operand) / 8;
}

// Returns the appropriately-sized Gp register for a given operand, respecting
// the instruction's OperandSizeType property.
arch::Gp getReg(const Instruction* instr, const lir::Operand* operand) {
  JIT_CHECK(
      operand->isReg(),
      "Expected a register for getReg '{}' in '{}'",
      *operand,
      *instr);
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
arch::Mem getMem(const Instruction* instr, const lir::Operand* operand) {
#if defined(CINDER_X86_64)
  int size = getOperandSizeInBytes(instr, operand);
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

asmjit::Imm getImm(const lir::Operand* operand) {
  return asmjit::Imm(operand->getConstant());
}

asmjit::Label getLabel(Environ* env, const lir::Operand* operand) {
  if (operand->getDefine()->hasAsmLabel()) {
    return operand->getDefine()->getAsmLabel();
  }
  return map_get(env->block_label_map, operand->getBasicBlock());
}

namespace {

#if defined(CINDER_AARCH64)

// Address the frame slot at |loc| (a negative offset from FP), preferring
// SP-relative addressing so that the access usually needs no scratch base.
asmjit::a64::Mem getStackSlotPtr(
    Environ* env,
    int32_t loc,
    const asmjit::a64::Gp& scratch = arch::reg_scratch_0,
    arch::AccessSize access_size = arch::AccessSize::k64) {
  if (env->sp_to_fp_delta != arch::kSpPositionUnknown) {
    JIT_DCHECK(
        loc < 0, "Frame slot offsets must be negative FP offsets, got {}", loc);
    JIT_DCHECK(
        loc + env->sp_to_fp_delta >= 0,
        "SP-relative frame slot at {} must not be below SP (delta {})",
        loc,
        env->sp_to_fp_delta);
    auto opt =
        arch::ptr_offset_try(a64::sp, loc + env->sp_to_fp_delta, access_size);
    if (opt.has_value()) {
      return opt.value();
    }
  }
  return ptr_resolve(env->as, arch::fp, loc, scratch, access_size);
}

#endif

void fillLiveValueLocations(
    CodeRuntime* code_runtime,
    std::size_t deopt_idx,
    const Instruction* instr,
    size_t begin_input,
    size_t end_input) {
  DeoptMetadata& deopt_meta = code_runtime->getDeoptMetadata(deopt_idx);
  for (size_t i = begin_input; i < end_input; i++) {
    auto loc = instr->getInput(i)->getPhyRegOrStackSlot();
    JIT_THROW_IF(
        loc.isFpRegister(),
        "Deopt live value {} of {} is in vector register {}, which the deopt "
        "trampoline does not spill",
        i - begin_input,
        instr->opname(),
        loc.toString());
    deopt_meta.live_values[i - begin_input].location = loc;
  }
}

void fillCallSiteLiveValueLocations(Environ* env, const Instruction* instr) {
  if constexpr (!kFreeThreadedBuild) {
    return;
  }
  auto it = env->callsite_live_value_metadata.find(instr);
  if (it == env->callsite_live_value_metadata.end()) {
    const hir::Instr* hir_instr = instr->origin();
    // Assume if there is no HIR instruction then this site does not allow
    // arbitrary execution.
    if (hir_instr != nullptr && hir_instr->asDeoptBase() == nullptr) {
      JIT_CHECK(
          hir_instr->asCallSiteLiveValuesBase() == nullptr,
          "Missing callsite live-value metadata for '{}'",
          *hir_instr);
    }
    return;
  }

  const Environ::CallSiteLiveValueMetadata& metadata = it->second;
  JIT_CHECK(
      metadata.live_values_instr != nullptr,
      "Missing callsite live-value instruction");
  DeoptMetadata& deopt_meta =
      env->code_rt->getDeoptMetadata(metadata.deopt_meta_index);
  JIT_CHECK(
      deopt_meta.live_values.size() ==
          metadata.live_values_instr->getNumInputs(),
      "Callsite live-value count mismatch");
  fillLiveValueLocations(
      env->code_rt,
      metadata.deopt_meta_index,
      metadata.live_values_instr,
      0,
      metadata.live_values_instr->getNumInputs());
}

} // namespace

#if defined(CINDER_AARCH64)
void translateA64GuardCC(Environ* env, const Instruction* instr) {
  auto index = static_cast<size_t>(instr->getInput(1)->getConstant());
  auto* block = map_get(env->deopt_exit_blocks, index);
  auto label = map_get(env->block_label_map, block);
  auto cond = static_cast<lir::Condition>(instr->getInput(0)->getConstant());

  emitBranchCC(env->as, cond, label);
  fillLiveValueLocations(env->code_rt, index, instr, 2, instr->getNumInputs());
}
#endif

// Translate GUARD instruction
void translateGuard(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  auto as = env->as;

  // the first four operands of the guard instruction are:
  //   * kind
  //   * deopt meta id
  //   * guard var (physical register) (0 for AlwaysFail)
  //   * target (for GuardIs and GuardType, and 0 for all others)

  auto index = static_cast<size_t>(instr->getInput(1)->getConstant());
  auto* deopt_block = map_get(env->deopt_exit_blocks, index);
  auto deopt_label = map_get(env->block_label_map, deopt_block);
  auto kind = instr->getInput(0)->getConstant();

  arch::Gp reg = x86::rax;
  bool is_double = false;
  if (kind != kAlwaysFail) {
    if (instr->getInput(2)->dataType() == jit::lir::Operand::kDouble) {
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
      case kHasType:
        emit_cmp(x86::qword_ptr(reg, offsetof(PyObject, ob_type)));
        as->jne(deopt_label);
        break;
    }
  }
#elif defined(CINDER_AARCH64)
  auto as = env->as;

  // the first four operands of the guard instruction are:
  //   * kind
  //   * deopt meta id
  //   * guard var (physical register) (0 for AlwaysFail)
  //   * target (for GuardIs and GuardType, and 0 for all others)

  auto index = static_cast<size_t>(instr->getInput(1)->getConstant());
  auto* deopt_block = map_get(env->deopt_exit_blocks, index);
  auto deopt_label = map_get(env->block_label_map, deopt_block);
  auto kind = instr->getInput(0)->getConstant();

  arch::Gp reg = arch::reg_scratch_0;
  uint64_t mask = 0;
  size_t sign_bit = 0;
  if (kind != kAlwaysFail) {
    auto data_type = instr->getInput(2)->dataType();
    if (data_type == jit::lir::Operand::k8bit) {
      mask = 0xFF;
      sign_bit = 7;
      // aarch64 doesn't have 8-bit registers, use 32-bit w register.
      reg = asmjit::a64::w(instr->getInput(2)->getPhyRegister().loc);
    } else if (data_type == jit::lir::Operand::k16bit) {
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

  // skip the first four inputs in Guard, which are
  // kind, deopt_meta id, guard var, and target.
  fillLiveValueLocations(env->code_rt, index, instr, 4, instr->getNumInputs());

  // Pair this post-call guard with the preceding call's return-address label
  // for the callsite->deopt-exit map used by deoptAllJitFramesOnStack().
  if (!env->pending_debug_locs.empty() && instr->origin() != nullptr &&
      env->pending_debug_locs.back().instr == instr->origin()) {
    env->callsite_deopt_pending.emplace_back(
        env->pending_debug_locs.back().label, deopt_label);
  }
}

void TranslateDeoptPatchpoint(Environ* env, const Instruction* instr) {
  auto as = env->as;

  auto patcher =
      reinterpret_cast<JumpPatcher*>(instr->getInput(0)->getMemoryAddress());

  // Generate patchpoint by writing in an appropriately sized nop.  As a future
  // optimization, we may be able to avoid reserving space for the patchpoint if
  // we can prove that the following bytes are not the target of a jump.
  // On x86, align the patchpoint to 8 bytes so the patch-point doesn't straddle
  // a cache line boundary. This is enough to make updates appear atomic to
  // other cores.
  //
  // Not needed on Arm as fixed instructions are a fixed size and updates
  // naturally atomic.
  if constexpr (kFreeThreadedBuild && kBuildArch == Arch::kX86_64) {
    as->align(AlignMode::kCode, 8);
  }
  auto patchpoint_label = as->newLabel();
  as->bind(patchpoint_label);

  auto stored_bytes = patcher->storedBytes();
  as->embed(stored_bytes.data(), stored_bytes.size());

  // Fill in deopt metadata
  auto index = static_cast<size_t>(instr->getInput(1)->getConstant());
  // skip the first two inputs which are the patcher and deopt metadata id
  fillLiveValueLocations(env->code_rt, index, instr, 2, instr->getNumInputs());
  auto* deopt_block = map_get(env->deopt_exit_blocks, index);
  auto deopt_label = map_get(env->block_label_map, deopt_block);

  // The runtime will link the patcher to the appropriate point in the code
  // once code generation has completed.
  env->pending_deopt_patchers.emplace_back(
      patcher, patchpoint_label, deopt_label);
}

void TranslateCompare(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  auto as = env->as;
  const lir::Operand* inp0 = instr->getInput(0);
  const lir::Operand* inp1 = instr->getInput(1);

  if (inp1->isImm() || inp1->isMem()) {
    as->cmp(AutoTranslator::getGp(inp0), inp1->getConstantOrAddress());
  } else if (!inp1->isVecD()) {
    as->cmp(AutoTranslator::getGp(inp0), AutoTranslator::getGp(inp1));
  } else {
    // Floating-point comparison; both operands are in XMM registers.  `comisd`
    // sets the flags in the unsigned sense (CF/ZF) and reports unordered (NaN)
    // operands as CF=ZF=PF=1; the setcc below then reads those flags.
    // NaN-correctness and the comparison direction are chosen when the compare
    // is lowered to LIR, so a compare fused into a branch, which reuses these
    // flags via compareToBranchCC on the LIR opcode, stays consistent with the
    // standalone setcc emitted here.
    as->comisd(AutoTranslator::getVecD(inp0), AutoTranslator::getVecD(inp1));
  }
  auto output = AutoTranslator::getGp(instr->output());
  as->set(asmCondCode(instr->condition()), output);
  if (instr->output()->dataType() != lir::Operand::k8bit) {
    as->movzx(
        AutoTranslator::getGp(instr->output()),
        asmjit::x86::gpb(instr->output()->getPhyRegister().loc));
  }
#elif defined(CINDER_AARCH64)
  auto as = env->as;
  const lir::Operand* inp0 = instr->getInput(0);
  const lir::Operand* inp1 = instr->getInput(1);

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
    // Floating-point comparison, see the note in the x86-64 path.  `fcmp` sets
    // NZCV (unordered/NaN operands set C=1, V=1 while leaving Z=0), and the
    // cset below reads them. NaN-correctness and the comparison
    // direction are chosen when the compare is lowered to LIR, keeping the
    // standalone cset and any fused b.cc consistent.
    as->fcmp(AutoTranslator::getVecD(inp0), AutoTranslator::getVecD(inp1));
  }

  auto output = AutoTranslator::getGpOutput(instr->output());
  as->cset(output, asmCondCode(instr->condition()));
#else
  CINDER_UNSUPPORTED
#endif
}

void translateIntToBool(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  x86::Builder* as = env->as;
  const lir::Operand* input = instr->getInput(0);
  x86::Gp output = AutoTranslator::getGp(instr->output());
  JIT_CHECK(
      instr->output()->dataType() == lir::Operand::k8bit,
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
  const lir::Operand* input = instr->getInput(0);
  a64::Gp output = AutoTranslator::getGpOutput(instr->output());
  JIT_CHECK(
      instr->output()->dataType() == lir::Operand::k8bit,
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
    Environ* env,
    const Instruction* instr,
    PhyLocation sent_in_source_loc,
    arch::Gp tstate_reg) {
  arch::Builder* as = env->as;
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
  as->str(tstate_reg, getStackSlotPtr(env, tstate.loc));

  const lir::Operand* target = instr->output();

  if (target->isStack()) {
    as->str(
        a64::x(sent_in_source_loc.loc),
        getStackSlotPtr(env, target->getStackSlot().loc));
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
    as->bl(_PyThreadState_GetCurrent);
    if (dst.id() != a64::x0.id()) {
      as->mov(dst, a64::x0);
    }
  }

  if (output->isStack()) {
    as->str(dst, getStackSlotPtr(env, output->getStackSlot().loc));
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

void translateResumeGenYield(Environ* env, const Instruction* instr) {
#if defined(CINDER_X86_64)
  arch::Builder* as = env->as;

  // Resumed execution in this generator begins here
  as->bind(env->pending_yield_resume_label);

  // Sent in value and tstate arrive in the argument registers for the
  // GenResumeFunc signature: arg[1] = sent value, arg[3] = tstate.
  emitLoadResumedYieldInputs(
      env, instr, ARGUMENT_REGS[1], x86::gpq(ARGUMENT_REGS[3].loc));
#elif defined(CINDER_AARCH64)
  a64::Builder* as = env->as;

  // Resumed execution in this generator begins here
  as->bind(env->pending_yield_resume_label);

  // Sent in value is in x1, and tstate is in x3 from resume entry-point args
  emitLoadResumedYieldInputs(env, instr, X1, a64::x3);
#else
  CINDER_UNSUPPORTED
#endif
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

// Lower LIR ReserveStack to a LEA (x86-64) or ADD (aarch64) that computes
// the address of the reserved stack region. The reserved data lives at
// [SP + max_arg_buffer_size], above the call argument buffer, so that
// function calls don't clobber it.
void translateReserveStack(Environ* env, const Instruction* instr) {
  auto* as = env->as;
  auto output = instr->output();
  JIT_CHECK(output->isReg(), "Expected output to be a register");

  int offset = env->max_arg_buffer_size;

#if defined(CINDER_X86_64)
  as->lea(x86::gpq(output->getPhyRegister().loc), x86::ptr(x86::rsp, offset));
#elif defined(CINDER_AARCH64)
  arch::add_signed_immediate(
      as, a64::x(output->getPhyRegister().loc), a64::sp, offset);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateEpilogueEnd(Environ* env, const Instruction* instr) {
  auto* as = env->as;

  auto* ret_val = instr->getInput(0);
  bool is_primitive = ret_val->dataType() != DataType::kObject &&
      ret_val->dataType() != DataType::kObjectUntagged;
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
  if (!saved_regs.empty()) {
    JIT_CHECK(
        env->last_callee_saved_reg_off != -1,
        "offset to callee saved regs not initialized");
    // Point rsp at the bottom of the callee-saved area.
    as->lea(x86::rsp, x86::ptr(x86::rbp, -env->last_callee_saved_reg_off));
    if constexpr (kOS == OS::kWindows) {
      // On Windows, callee-saved XMM registers were saved with movaps and
      // must be restored the same way. GP registers are restored with pop.
      auto vecd_regs = saved_regs & ALL_VECD_REGISTERS;
      auto gp_regs = saved_regs & ALL_GP_REGISTERS;
      int xmm_offset = 0;
      while (!vecd_regs.empty()) {
        auto reg = vecd_regs.getFirst();
        as->movaps(
            x86::xmm(reg.loc - VECD_REG_BASE), x86::ptr(x86::rsp, xmm_offset));
        xmm_offset += kVecDSize;
        vecd_regs.removeFirst();
      }
      int vecd_count = (saved_regs & ALL_VECD_REGISTERS).count();
      int gp_count = gp_regs.count();
      int vecd_area_size = vecd_count * kVecDSize;
      if (vecd_count > 0 && (gp_count * kPointerSize) % kStackAlign != 0) {
        vecd_area_size += kPointerSize;
      }
      if (vecd_area_size > 0) {
        as->add(x86::rsp, vecd_area_size);
      }
      while (!gp_regs.empty()) {
        as->pop(x86::gpq(gp_regs.getLast().loc));
        gp_regs.removeLast();
      }
    } else {
      // Pop in reverse push order (GetLast→GetFirst) to restore registers.
      while (!saved_regs.empty()) {
        as->pop(x86::gpq(saved_regs.getLast().loc));
        saved_regs.removeLast();
      }
    }
  }
  as->leave();
  as->ret();
#elif defined(CINDER_AARCH64)
  // Move return value to ABI return register
  if (is_double) {
    if (ret_val->isStack()) {
      as->ldr(a64::d0, getStackSlotPtr(env, ret_val->getStackSlot().loc));
    } else if (
        ret_val->isReg() &&
        ret_val->getPhyRegister().loc != arch::reg_double_return_loc.loc) {
      as->fmov(a64::d0, a64::d(ret_val->getPhyRegister().loc - VECD_REG_BASE));
    }
  } else {
    if (ret_val->isStack()) {
      as->ldr(a64::x0, getStackSlotPtr(env, ret_val->getStackSlot().loc));
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
  if (!saved_regs.empty()) {
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
    if (!gp_regs.empty()) {
      if (gp_regs.count() % 2 == 1) {
        as->ldr(
            a64::x(gp_regs.getFirst().loc),
            a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        gp_regs.removeFirst();
        offset += 16;
      }
      while (!gp_regs.empty()) {
        auto first = a64::x(gp_regs.getFirst().loc);
        gp_regs.removeFirst();
        auto second = a64::x(gp_regs.getFirst().loc);
        gp_regs.removeFirst();
        as->ldp(first, second, a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        offset += 16;
      }
    }

    // Restore VecD registers (iterate GetFirst→GetLast, same as save).
    if (!vecd_regs.empty()) {
      if (vecd_regs.count() % 2 == 1) {
        as->ldr(
            a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE),
            a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        vecd_regs.removeFirst();
        offset += 16;
      }
      while (!vecd_regs.empty()) {
        auto first = a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE);
        vecd_regs.removeFirst();
        auto second = a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE);
        vecd_regs.removeFirst();
        as->ldp(first, second, a64::ptr(arch::reg_scratch_0, -(offset + 16)));
        offset += 16;
      }
    }
  }
  as->mov(a64::sp, arch::fp);
  as->ldp(arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
  as->ret(arch::lr);
  env->sp_to_fp_delta = arch::kSpPositionUnknown;
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
  // Allocate header + spill space, then save callee-saved registers.
  asmjit::BaseNode* alloc_cursor = as->cursor();
  as->sub(x86::rsp, env->resume_header_and_spill_size);
  env->addAnnotation(std::string("Allocate stack frame"), alloc_cursor);

  asmjit::BaseNode* save_cursor = as->cursor();
  auto gp_saved_regs = env->resume_saved_regs & ALL_GP_REGISTERS;
  // Push GP callee-saved registers (1-2B per register).
  while (!gp_saved_regs.empty()) {
    as->push(x86::gpq(gp_saved_regs.getFirst().loc));
    gp_saved_regs.removeFirst();
  }

  auto gp_save_count = (env->resume_saved_regs & ALL_GP_REGISTERS).count();
  if constexpr (kOS == OS::kWindows) {
    auto vecd_saved_regs = env->resume_saved_regs & ALL_VECD_REGISTERS;
    auto vecd_save_count = vecd_saved_regs.count();

    // On Windows, callee-saved XMM registers (XMM6-XMM15) are saved via movaps
    // into the stack space between the GP pushes and the arg buffer.
    // Compute the offset where XMM saves start (right after GP pushes,
    // aligned).
    int vecd_area_size = vecd_save_count * kVecDSize;
    if (vecd_save_count > 0 &&
        (gp_save_count * kPointerSize) % kStackAlign != 0) {
      vecd_area_size += kPointerSize; // alignment padding
    }
    int arg_buffer_size = env->resume_frame_total_size -
        env->resume_header_and_spill_size - gp_save_count * kPointerSize -
        vecd_area_size;
    if (vecd_area_size + arg_buffer_size > 0) {
      as->sub(x86::rsp, vecd_area_size + arg_buffer_size);
    }
    // Save XMM registers into [rsp + arg_buffer_size + offset]
    int xmm_offset = arg_buffer_size;
    while (!vecd_saved_regs.empty()) {
      auto reg = vecd_saved_regs.getFirst();
      as->movaps(
          x86::ptr(x86::rsp, xmm_offset), x86::xmm(reg.loc - VECD_REG_BASE));
      xmm_offset += kVecDSize;
      vecd_saved_regs.removeFirst();
    }
  } else {
    int arg_buffer_size = env->resume_frame_total_size -
        env->resume_header_and_spill_size - gp_save_count * kPointerSize;
    if (arg_buffer_size > 0) {
      as->sub(x86::rsp, arg_buffer_size);
    }
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
  // A generator's body runs with FP pointing at its heap-allocated
  // GenDataFooter rather than at the machine stack (see
  // LIRGenerator::emitLoadFrame), so there is no delta to track and SP-relative
  // frame slots would address unrelated memory. The FP swap already invalidates
  // the delta via the SP/FP write guard in AutoTranslator::translateInstr, but
  // never establish one in the first place so that a future change which
  // re-establishes it mid-body can't silently resurrect the hazard.
  env->sp_to_fp_delta = env->is_generator ? arch::kSpPositionUnknown
                                          : env->resume_frame_total_size;
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
  if (!gp_regs.empty()) {
    if (gp_regs.count() % 2 == 1) {
      as->str(
          a64::x(gp_regs.getFirst().loc),
          a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      gp_regs.removeFirst();
      reg_offset += 16;
    }
    while (!gp_regs.empty()) {
      auto first = a64::x(gp_regs.getFirst().loc);
      gp_regs.removeFirst();
      auto second = a64::x(gp_regs.getFirst().loc);
      gp_regs.removeFirst();
      as->stp(first, second, a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
  if (!vecd_regs.empty()) {
    if (vecd_regs.count() % 2 == 1) {
      as->str(
          a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE),
          a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      vecd_regs.removeFirst();
      reg_offset += 16;
    }
    while (!vecd_regs.empty()) {
      auto first = a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE);
      vecd_regs.removeFirst();
      auto second = a64::d(vecd_regs.getFirst().loc - VECD_REG_BASE);
      vecd_regs.removeFirst();
      as->stp(first, second, a64::ptr(arch::reg_scratch_0, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
  env->addAnnotation(std::string("Save callee-saved registers"), save_cursor);
#else
  CINDER_UNSUPPORTED
#endif
}

// Emit a branch through a memory-indirect operand [base + offset].
// Used by kBranch when its input is a MemoryIndirect operand.
void translateBranchIndirect(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  const lir::Operand* input = instr->getInput(0);

  if (input->isReg()) {
#if defined(CINDER_X86_64)
    as->jmp(AutoTranslator::getGp(input));
#elif defined(CINDER_AARCH64)
    as->br(AutoTranslator::getGp(input));
#else
    CINDER_UNSUPPORTED
#endif
    return;
  }

  JIT_CHECK(
      input->isInd(),
      "Branch indirect input must be memory indirect or register");

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

// Emit a variadic sequence of GP register pushes (x86) or stp pairs (aarch64).
// Each input operand is a physical register to save. The registers are stored
// in input order (first input is the lowest address).
void translateVariadicPush(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  size_t n = instr->getNumInputs();

#if defined(CINDER_X86_64)
  for (size_t i = 0; i < n; i++) {
    as->push(x86::gpq(instr->getInput(n - i - 1)->getPhyRegister().loc));
  }
#elif defined(CINDER_AARCH64)
  // First pair uses pre-index to allocate stack space for all pairs.
  // Remaining pairs use offset addressing within the allocated region.
  constexpr int bytes_per_store = 16;
  int total_pairs = (n + 1) / 2;
  int alloc = total_pairs * bytes_per_store;
  size_t i = 0;

  // First pair: pre-index allocation
  if (n >= 2) {
    as->stp(
        a64::x(instr->getInput(0)->getPhyRegister().loc),
        a64::x(instr->getInput(1)->getPhyRegister().loc),
        a64::ptr_pre(a64::sp, -alloc));
    i = 2;
  } else if (n == 1) {
    as->str(
        a64::x(instr->getInput(0)->getPhyRegister().loc),
        a64::ptr_pre(a64::sp, -alloc));
    i = 1;
  }

  // Remaining pairs at positive offsets from sp.
  int pair_idx = 1;
  while (i + 1 < n) {
    as->stp(
        a64::x(instr->getInput(i)->getPhyRegister().loc),
        a64::x(instr->getInput(i + 1)->getPhyRegister().loc),
        a64::ptr(a64::sp, pair_idx * bytes_per_store));
    i += 2;
    pair_idx++;
  }
  if (i < n) {
    as->str(
        a64::x(instr->getInput(i)->getPhyRegister().loc),
        a64::ptr(a64::sp, pair_idx * bytes_per_store));
  }

  env->adjustSp(alloc);
#else
  CINDER_UNSUPPORTED
#endif
}

// Store a pair of GP register values at consecutive pointer-sized slots.
// Input 0: immediate offset. Input 1: base register.
// Inputs 2, 3: values stored at [base+offset] and [base+offset+8].
#if defined(CINDER_AARCH64)

// Resolve the address of a load/store pair, preferring the single-instruction
// stp/ldp form. Returns nullopt when the pair can't be encoded and the caller
// has to fall back to two separate accesses.
//
// A frame-pointer-relative pair is also reachable from SP while the frame is
// established (see getStackSlotPtr), and the two bases have very different
// reach: the scaled 7-bit offset covers -512..504 either way, but the SP form
// measures from the other end of the frame, so one can encode where the other
// can't. Both are tried before giving up.
std::optional<asmjit::a64::Mem>
getPairPtr(Environ* env, PhyLocation base_reg, int32_t offset) {
  auto encodable = [](int32_t off) {
    return (off & (kPointerSize - 1)) == 0 && Support::isInt7(off >> 3);
  };

  if (base_reg == arch::reg_frame_pointer_loc) {
    if (env->sp_to_fp_delta != arch::kSpPositionUnknown) {
      // pairMemoryLocation() reports the same base for real frame slots and for
      // genuine FP-relative indirects (which a generator uses to reach its
      // GenDataFooter). Only the former may be rewritten, and the delta is only
      // ever known while FP is a real frame pointer, so assert that here.
      JIT_DCHECK(
          offset < 0,
          "Frame slot offsets must be negative FP offsets, got {}",
          offset);
      int32_t sp_offset = offset + env->sp_to_fp_delta;
      JIT_DCHECK(
          sp_offset >= 0,
          "SP-relative frame slot at {} must not be below SP (delta {})",
          offset,
          env->sp_to_fp_delta);
      if (encodable(sp_offset)) {
        return a64::ptr(a64::sp, sp_offset);
      }
    }
  }

  if (encodable(offset)) {
    auto base = base_reg == SP ? a64::sp : a64::x(base_reg.loc);
    return a64::ptr(base, offset);
  }

  return std::nullopt;
}

// Materialize base+offset in the scratch register so a pair whose offset is
// out of stp/ldp range can still be issued as a single instruction from
// [scratch].
//
// The two halves must not be resolved separately: each resolution recomputes
// an address into the same scratch, so the second one destroys a pair register
// that happens to be that scratch. A store would then write the address
// instead of its value, and a load would lose the value it had just read.
// pairAdjacentMemoryOps refuses to build a pair that would land here holding a
// scratch register, so the check below is a tripwire rather than a live case.
asmjit::a64::Mem getPairScratchPtr(
    Environ* env,
    PhyLocation base_reg,
    int32_t offset,
    PhyLocation reg0,
    PhyLocation reg1) {
  JIT_CHECK(
      reg0 != arch::reg_scratch_0_loc && reg1 != arch::reg_scratch_0_loc,
      "pair at offset {} holds the address scratch {} in a value/destination "
      "slot",
      offset,
      arch::reg_scratch_0_loc);
  auto base = base_reg == SP ? a64::sp : a64::x(base_reg.loc);
  arch::add_signed_immediate(env->as, arch::reg_scratch_0, base, offset);
  return a64::ptr(arch::reg_scratch_0);
}

#endif

void translateStorePair(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  JIT_DCHECK(
      instr->getNumInputs() == 4,
      "StorePair expects exactly 4 inputs (offset, base, val0, val1)");
  int32_t offset = static_cast<int32_t>(instr->getInput(0)->getConstant());

#if defined(CINDER_X86_64)
  auto base = x86::gpq(instr->getInput(1)->getPhyRegister().loc);
  as->mov(
      x86::qword_ptr(base, offset),
      x86::gpq(instr->getInput(2)->getPhyRegister().loc));
  as->mov(
      x86::qword_ptr(base, offset + kPointerSize),
      x86::gpq(instr->getInput(3)->getPhyRegister().loc));
#elif defined(CINDER_AARCH64)
  auto base_reg = instr->getInput(1)->getPhyRegister();
  auto val0_loc = instr->getInput(2)->getPhyRegister();
  auto val1_loc = instr->getInput(3)->getPhyRegister();
  auto val0 = a64::x(val0_loc.loc);
  auto val1 = a64::x(val1_loc.loc);

  if (auto ptr = getPairPtr(env, base_reg, offset)) {
    as->stp(val0, val1, *ptr);
  } else {
    as->stp(
        val0,
        val1,
        getPairScratchPtr(env, base_reg, offset, val0_loc, val1_loc));
  }
#else
  CINDER_UNSUPPORTED
#endif
}

void translateLoadPair(Environ* env, const Instruction* instr) {
  arch::Builder* as = env->as;
  JIT_DCHECK(
      instr->getNumInputs() == 3,
      "LoadPair expects exactly 3 inputs (offset, base, dst1)");
  int32_t offset = static_cast<int32_t>(instr->getInput(0)->getConstant());
  auto base_reg = instr->getInput(1)->getPhyRegister();
  auto dst0_loc = instr->output()->getPhyRegister();
  auto dst1_loc = instr->getInput(2)->getPhyRegister();

#if defined(CINDER_X86_64)
  auto base = x86::gpq(base_reg.loc);
  as->mov(x86::gpq(dst0_loc.loc), x86::qword_ptr(base, offset));
  as->mov(x86::gpq(dst1_loc.loc), x86::qword_ptr(base, offset + kPointerSize));
#elif defined(CINDER_AARCH64)
  auto dst0 = a64::x(dst0_loc.loc);
  auto dst1 = a64::x(dst1_loc.loc);

  if (auto ptr = getPairPtr(env, base_reg, offset)) {
    as->ldp(dst0, dst1, *ptr);
  } else {
    as->ldp(
        dst0,
        dst1,
        getPairScratchPtr(env, base_reg, offset, dst0_loc, dst1_loc));
  }
#else
  CINDER_UNSUPPORTED
#endif
}

// Tear down the frame. On x86, this executes 'leave' (mov rsp, rbp; pop rbp).
// On aarch64, this restores sp from fp and pops the frame record (fp + lr).
//
// No inputs.
void translateLeave(Environ* env) {
  arch::Builder* as = env->as;

#if defined(CINDER_X86_64)
  as->leave();
#elif defined(CINDER_AARCH64)
  as->mov(a64::sp, arch::fp);
  as->ldp(arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
  env->sp_to_fp_delta = arch::kSpPositionUnknown;
#else
  CINDER_UNSUPPORTED
#endif
}

// Return from a function. On x86, this is 'ret'. On aarch64, 'ret lr'.
//
// No inputs.
void translateRet(Environ* env) {
  arch::Builder* as = env->as;

#if defined(CINDER_X86_64)
  as->ret();
#elif defined(CINDER_AARCH64)
  as->ret(arch::lr);
#else
  CINDER_UNSUPPORTED
#endif
}

void translateShift(Environ* env, const Instruction* instr) {
  auto opcode = instr->opcode();
  auto in0_reg = getReg(instr, instr->getInput(0));
  auto in1 = instr->getInput(1);
  auto out_reg =
      (instr->getNumOutputs() > 0) ? getReg(instr, instr->output()) : in0_reg;
  // Currently just a limitation of x86-64 register allocation.
  JIT_CHECK(
      kBuildArch != Arch::kX86_64 || in1->isImm(),
      "Cannot emit non-immediate RHS for instruction '{}'",
      *instr);

  if (instr->getNumOutputs() > 0 && kBuildArch != Arch::kAarch64) {
    env->as->mov(out_reg, in0_reg);
  }

#if defined(CINDER_X86_64)
  asmjit::Imm shift = getImm(in1);
  switch (opcode) {
    case Opcode::kLShift:
      env->as->shl(out_reg, shift);
      return;
    case Opcode::kRShift:
      env->as->sar(out_reg, shift);
      return;
    case Opcode::kRShiftUn:
      env->as->shr(out_reg, shift);
      return;
    default:
      break;
  }
#elif defined(CINDER_AARCH64)
  switch (opcode) {
    case Opcode::kLShift:
      if (in1->isReg()) {
        env->as->lsl(out_reg, in0_reg, getReg(instr, in1));
      } else {
        env->as->lsl(out_reg, in0_reg, getImm(in1));
      }
      return;
    case Opcode::kRShift:
      if (in1->isReg()) {
        env->as->asr(out_reg, in0_reg, getReg(instr, in1));
      } else {
        env->as->asr(out_reg, in0_reg, getImm(in1));
      }
      return;
    case Opcode::kRShiftUn:
      if (in1->isReg()) {
        env->as->lsr(out_reg, in0_reg, getReg(instr, in1));
      } else {
        env->as->lsr(out_reg, in0_reg, getImm(in1));
      }
      return;
    default:
      break;
  }
#else
  JIT_ABORT("Unrecognized architecture for emitting shift instruction");
#endif
  JIT_ABORT("Unrecognized shift opcode '{}'", instr->opname());
}

#if defined(CINDER_AARCH64)
namespace {

using AT = AutoTranslator;

// We do not want to extend AT::getGp to support SP because we only want to
// return SP in very specific circumstances (e.g., building an address relative
// to SP).
arch::Gp getGpOrSP(const lir::Operand* operand) {
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
        indirect->getMultiplier());

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
    const MemoryIndirect* indirect,
    DataType data_type) {
  auto base = getGpOrSP(indirect->getBaseRegOperand());
  auto indexRegOperand = indirect->getIndexRegOperand();
  auto offset = indirect->getOffset();

  if (indexRegOperand != nullptr) {
    auto index = AT::getGp(indexRegOperand);
    auto multiplier = indirect->getMultiplier();

    if (offset == 0) {
      if (multiplier == 0) {
        return a64::ptr(base, index);
      } else if (multiplier == byteShift(data_type)) {
        return a64::ptr(base, index, a64::lsl(multiplier));
      }
    }

    leaIndex(as, scratch1, base, index, multiplier);

    base = scratch1;
  }

  return arch::ptr_resolve(as, base, offset, scratch0);
}

void loadToReg(
    arch::Builder* as,
    const lir::Operand* output,
    const arch::Mem& input) {
  if (output->isVecD()) {
    as->ldr(AT::getVecD(output), input);
  } else {
    switch (output->dataType()) {
      case lir::Operand::k8bit:
        as->ldrb(
            AT::getGp(DataType::k32bit, output->getPhyRegister().loc), input);
        break;
      case lir::Operand::k16bit:
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
    const lir::Operand* input,
    const lir::Operand* output_operand,
    const arch::Mem& output) {
  if (input->isVecD()) {
    as->str(AT::getVecD(input), output);
  } else {
    switch (output_operand->dataType()) {
      case lir::Operand::k8bit:
        as->strb(
            AT::getGp(DataType::k32bit, input->getPhyRegister().loc), output);
        break;
      case lir::Operand::k16bit:
        as->strh(
            AT::getGp(DataType::k32bit, input->getPhyRegister().loc), output);
        break;
      default:
        as->str(
            AT::getGp(output_operand->dataType(), input->getPhyRegister().loc),
            output);
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
        as, getGpOrSP(output), arch::fp, input->getStackSlot().loc);
  } else if (input->isMem()) {
    auto address = reinterpret_cast<uint64_t>(input->getMemoryAddress());
    as->mov(getGpOrSP(output), address);
  } else if (input->isInd()) {
    leaIndirect(as, getGpOrSP(output), input->getMemoryIndirect());
  } else if (input->isLabel()) {
    asmjit::Label label = input->getDefine()->hasAsmLabel()
        ? input->getDefine()->getAsmLabel()
        : map_get(env->block_label_map, input->getBasicBlock());
    as->adr(getGpOrSP(output), label);
  } else {
    JIT_ABORT("Unsupported operand type for Lea: {}", input->type());
  }
}

void translateCall(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  auto output = instr->output();
  auto input = instr->getInput(0);

  if (input->isImm()) {
    // Use bl(imm) so asmjit can pick the final encoding at relocation time:
    // direct bl if within ±128MB, or a branch to an out-of-line stub.
    as->bl(static_cast<uint64_t>(input->getConstant()));
  } else if (input->isReg()) {
    as->blr(AT::getGp(input));
  } else if (input->isStack()) {
    as->ldr(
        arch::reg_scratch_br, getStackSlotPtr(env, input->getStackSlot().loc));
    as->blr(arch::reg_scratch_br);
  } else if (input->isImm()) {
    as->mov(arch::reg_scratch_br, input->getConstant());
    as->blr(arch::reg_scratch_br);
  } else if (input->isLabel()) {
    asmjit::Label label = input->getDefine()->hasAsmLabel()
        ? input->getDefine()->getAsmLabel()
        : map_get(env->block_label_map, input->getBasicBlock());
    as->bl(label);
  } else {
    JIT_ABORT("Unsupported operand type for Call: {}", input->type());
  }

  if (instr->origin()) {
    asmjit::Label label = as->newLabel();
    as->bind(label);
    env->pending_debug_locs.emplace_back(label, instr->origin());
  }

  if (output->type() != lir::Operand::kNone) {
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

// Move now only handles register-to-register and immediate-to-register.
void translateMove(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const lir::Operand* output = instr->output();
  const lir::Operand* input = instr->getInput(0);

  // MoveRelaxed still supports memory, keep its old validation but for plain
  // Move we enforce register-only.
  if (instr->isMoveRelaxed()) {
    checkMoveRelaxedOperandShape(instr);
    // For MoveRelaxed we fall through to the generic memory-capable path
    // implemented in translateLoad/Store helpers below, but keep a quick
    // reg-reg path here for efficiency.
  } else {
    JIT_CHECK(
        output->isReg(),
        "Move output must be a register, got {} (use Store for memory)",
        output->type());
    JIT_CHECK(
        input->isReg() || input->isImm(),
        "Move input must be Reg or Imm, got {} (use Load for memory)",
        input->type());
  }

  // Register-to-register / immediate-to-register.
  if (output->type() == lir::OperandType::kReg) {
    switch (input->type()) {
      case lir::OperandType::kReg: {
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
        return;
      }
      case lir::OperandType::kImm: {
        auto constant = input->getConstant();
        if (output->isVecD()) {
          as->fmov(AT::getVecD(output), constant);
        } else if (constant == 0) {
          as->mov(
              AT::getGpWiden(output),
              AT::getGpWiden(output->dataType(), a64::xzr.id()));
        } else if (input->dataType() == lir::Operand::kObject) {
          as->load_addr(
              a64::x(output->getPhyRegister().loc),
              static_cast<uint64_t>(constant));
        } else {
          as->mov(AT::getGpWiden(output), constant);
        }
        return;
      }
      default:
        break;
    }
  }

  // If we reach here and it's MoveRelaxed with memory operands, handle via
  // the shared Load/Store logic below to avoid duplication.
  if (instr->isMoveRelaxed()) {
    if (output->isReg() && isMemoryMoveOperand(input)) {
      // Load path for MoveRelaxed (relaxed atomic load).
      if (output->isVecD()) {
        if (input->isStack()) {
          as->ldr(
              AT::getVecD(output),
              getStackSlotPtr(env, input->getStackSlot().loc));
        } else if (input->isInd()) {
          auto ptr = ptrIndirect(
              as,
              arch::reg_scratch_0,
              arch::reg_scratch_1,
              input->getMemoryIndirect(),
              output->dataType());
          loadToReg(as, output, ptr);
        } else {
          JIT_ABORT(
              "Unsupported operand type for MoveRelaxed load: Reg + {}",
              input->type());
        }
      } else {
        if (input->isStack()) {
          auto dst = a64::x(output->getPhyRegister().loc);
          auto ptr = getStackSlotPtr(env, input->getStackSlot().loc, dst);
          switch (output->dataType()) {
            case lir::Operand::k8bit:
              as->ldrb(AT::getGpOutput(output), ptr);
              break;
            case lir::Operand::k16bit:
              as->ldrh(AT::getGpOutput(output), ptr);
              break;
            default:
              as->ldr(AT::getGp(output), ptr);
              break;
          }
        } else if (input->isInd()) {
          auto ptr = ptrIndirect(
              as,
              arch::reg_scratch_0,
              arch::reg_scratch_1,
              input->getMemoryIndirect(),
              output->dataType());
          loadToReg(as, output, ptr);
        } else {
          JIT_ABORT(
              "Unsupported operand type for MoveRelaxed load: Reg + {}",
              input->type());
        }
      }
      return;
    } else if (isMemoryMoveOperand(output)) {
      auto scratch0 = arch::reg_scratch_0;
      auto scratch1 = arch::reg_scratch_1;
      if (output->type() == lir::OperandType::kStack) {
        auto scratch = input->getPhyRegister() == arch::reg_scratch_0_loc
            ? arch::reg_scratch_1
            : arch::reg_scratch_0;
        storeFromReg(
            as,
            input,
            output,
            getStackSlotPtr(env, output->getStackSlot().loc, scratch));
        return;
      } else if (output->type() == lir::OperandType::kMem) {
        as->load_addr(scratch0, output->getMemoryAddress());
        if (input->isReg()) {
          if (input->isVecD()) {
            as->str(AT::getVecD(input), a64::ptr(scratch0));
          } else {
            as->str(AT::getGpWiden(input), a64::ptr(scratch0));
          }
        } else if (input->isImm()) {
          as->mov(scratch1, input->getConstant());
          as->str(scratch1, a64::ptr(scratch0));
        } else {
          JIT_ABORT(
              "Unsupported operand type for MoveRelaxed store: Mem + {}",
              input->type());
        }
        return;
      } else if (output->type() == lir::OperandType::kInd) {
        if (input->isReg()) {
          auto ptr = ptrIndirect(
              as,
              scratch0,
              scratch1,
              output->getMemoryIndirect(),
              output->dataType());
          storeFromReg(as, input, output, ptr);
        } else if (input->isImm()) {
          auto ptr = ptrIndirect(
              as,
              scratch0,
              scratch1,
              output->getMemoryIndirect(),
              output->dataType());
          switch (output->dataType()) {
            case lir::Operand::k8bit:
              as->mov(a64::w(scratch1.id()), input->getConstant());
              as->strb(a64::w(scratch1.id()), ptr);
              break;
            case lir::Operand::k16bit:
              as->mov(a64::w(scratch1.id()), input->getConstant());
              as->strh(a64::w(scratch1.id()), ptr);
              break;
            case lir::Operand::k32bit:
              as->mov(a64::w(scratch1.id()), input->getConstant());
              as->str(a64::w(scratch1.id()), ptr);
              break;
            default:
              as->mov(scratch1, input->getConstant());
              as->str(scratch1, ptr);
              break;
          }
        } else {
          JIT_ABORT(
              "Unsupported operand type for MoveRelaxed store: Ind + {}",
              input->type());
        }
        return;
      }
    }
  }

  JIT_ABORT(
      "Unsupported operand type for Move: {} + {}",
      output->type(),
      input->type());
}

void translateLoad(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;
  const lir::Operand* output = instr->output();
  const lir::Operand* input = instr->getInput(0);

  JIT_CHECK(
      output->isReg(),
      "Load output must be a register, got {}",
      output->type());
  JIT_CHECK(
      isMemoryMoveOperand(input),
      "Load input must be memory (Stk/Mem/Ind), got {}",
      input->type());

  switch (input->type()) {
    case lir::OperandType::kStack: {
      if (output->isVecD()) {
        as->ldr(
            AT::getVecD(output),
            getStackSlotPtr(env, input->getStackSlot().loc));
      } else {
        auto dst = a64::x(output->getPhyRegister().loc);
        auto ptr = getStackSlotPtr(env, input->getStackSlot().loc, dst);
        switch (output->dataType()) {
          case lir::Operand::k8bit:
            as->ldrb(AT::getGpOutput(output), ptr);
            break;
          case lir::Operand::k16bit:
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
      auto ptr = ptrIndirect(
          as,
          arch::reg_scratch_0,
          arch::reg_scratch_1,
          input->getMemoryIndirect(),
          output->dataType());
      loadToReg(as, output, ptr);
      break;
    }
    case lir::OperandType::kMem: {
      // AArch64 has no direct absolute load; materialize address then load.
      auto scratch0 = arch::reg_scratch_0;
      as->load_addr(scratch0, input->getMemoryAddress());
      if (output->isVecD()) {
        as->ldr(AT::getVecD(output), a64::ptr(scratch0));
      } else {
        switch (output->dataType()) {
          case lir::Operand::k8bit:
            as->ldrb(AT::getGpOutput(output), a64::ptr(scratch0));
            break;
          case lir::Operand::k16bit:
            as->ldrh(AT::getGpOutput(output), a64::ptr(scratch0));
            break;
          default:
            as->ldr(AT::getGp(output), a64::ptr(scratch0));
            break;
        }
      }
      break;
    }
    default:
      JIT_ABORT("Unsupported operand type for Load: Reg + {}", input->type());
  }
}

void translateStore(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;
  auto scratch0 = arch::reg_scratch_0;
  auto scratch1 = arch::reg_scratch_1;

  const lir::Operand* output = instr->output();
  const lir::Operand* input = instr->getInput(0);

  JIT_CHECK(
      isMemoryMoveOperand(output),
      "Store output must be memory (Stk/Mem/Ind), got {}",
      output->type());
  JIT_CHECK(
      input->isReg() || input->isImm(),
      "Store input must be Reg or Imm, got {}",
      input->type());

  switch (output->type()) {
    case lir::OperandType::kStack: {
      if (!input->isReg()) {
        // For Imm store to stack, we need to materialize via scratch.
        as->mov(scratch1, input->getConstant());
        auto ptr = getStackSlotPtr(env, output->getStackSlot().loc, scratch1);
        switch (output->dataType()) {
          case lir::Operand::k8bit:
            as->strb(a64::w(scratch1.id()), ptr);
            break;
          case lir::Operand::k16bit:
            as->strh(a64::w(scratch1.id()), ptr);
            break;
          case lir::Operand::k32bit:
            as->str(a64::w(scratch1.id()), ptr);
            break;
          default:
            as->str(scratch1, ptr);
            break;
        }
      } else {
        auto scratch = input->getPhyRegister() == arch::reg_scratch_0_loc
            ? arch::reg_scratch_1
            : arch::reg_scratch_0;
        storeFromReg(
            as,
            input,
            output,
            getStackSlotPtr(env, output->getStackSlot().loc, scratch));
      }
      break;
    }
    case lir::OperandType::kMem: {
      as->load_addr(scratch0, output->getMemoryAddress());
      if (input->isReg()) {
        if (input->isVecD()) {
          as->str(AT::getVecD(input), a64::ptr(scratch0));
        } else {
          as->str(AT::getGpWiden(input), a64::ptr(scratch0));
        }
      } else {
        as->mov(scratch1, input->getConstant());
        as->str(scratch1, a64::ptr(scratch0));
      }
      break;
    }
    case lir::OperandType::kInd: {
      if (input->isReg()) {
        auto ptr = ptrIndirect(
            as,
            scratch0,
            scratch1,
            output->getMemoryIndirect(),
            output->dataType());
        storeFromReg(as, input, output, ptr);
      } else {
        auto ptr = ptrIndirect(
            as,
            scratch0,
            scratch1,
            output->getMemoryIndirect(),
            output->dataType());
        switch (output->dataType()) {
          case lir::Operand::k8bit:
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->strb(a64::w(scratch1.id()), ptr);
            break;
          case lir::Operand::k16bit:
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->strh(a64::w(scratch1.id()), ptr);
            break;
          case lir::Operand::k32bit:
            as->mov(a64::w(scratch1.id()), input->getConstant());
            as->str(a64::w(scratch1.id()), ptr);
            break;
          default:
            as->mov(scratch1, input->getConstant());
            as->str(scratch1, ptr);
            break;
        }
      }
      break;
    }
    default:
      JIT_ABORT(
          "Unsupported output operand type for Store: {}", output->type());
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
  const lir::Operand* input = instr->getInput(0);
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
    // Address goes in the destination, not the shared scratch; see the kStack
    // load in translateMove for why. Each of these emits a single load, so the
    // address is consumed before the destination is written.
    auto dst = a64::x(output.id());

    switch (input_size) {
      case 8:
        emit_load8(
            as, output, getStackSlotPtr(env, loc, dst, arch::AccessSize::k8));
        break;
      case 16:
        emit_load16(
            as, output, getStackSlotPtr(env, loc, dst, arch::AccessSize::k16));
        break;
      case 32:
        as->ldr(
            a64::w(output.id()),
            getStackSlotPtr(env, loc, dst, arch::AccessSize::k32));
        break;
      default:
        JIT_ABORT("Unsupported input size for {}: {}", opname, input_size);
    }
  } else {
    JIT_ABORT("Unsupported operand type for {}: {}", opname, input->type());
  }
}

void translateZext(Environ* env, const Instruction* instr) {
  // ARM64 uxtb/uxth/ldrb/ldrh only accept W-register destinations.
  // Writing to W implicitly zeros the upper 32 bits of the X register,
  // so this correctly zero-extends to 64 bits even for k64bit outputs.
  translateMovExtOp(
      env,
      instr,
      "Zext",
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

void translateSext(Environ* env, const Instruction* instr) {
  // The shared helper's 32-bit path only zero-extends, so sign-extending from
  // 32 bits needs sxtw/ldrsw here.
  const lir::Operand* input = instr->getInput(0);
  if (input->sizeInBits() == 32) {
    a64::Builder* as = env->as;

    JIT_THROW_IF(
        instr->output()->sizeInBits() != 64,
        "Sign-extend from 32-bits should always go to 64-bits, got '{}' "
        "instead",
        *instr);
    auto output = AT::getGpOutput(instr->output());

    if (input->isReg()) {
      as->sxtw(output, asmjit::a64::w(input->getPhyRegister().loc));
    } else if (input->isStack()) {
      auto loc = input->getStackSlot().loc;
      as->ldrsw(
          output,
          arch::ptr_resolve(
              as, arch::fp, loc, arch::reg_scratch_0, arch::AccessSize::k32));
    } else {
      JIT_THROW("Unsupported operand type for '{}'", *instr);
    }
    return;
  }

  translateMovExtOp(
      env,
      instr,
      "Sext",
      [](a64::Builder* as, auto... args) { as->sxtb(args...); },
      [](a64::Builder* as, auto... args) { as->sxth(args...); },
      [](a64::Builder* as, auto... args) { as->ldrsb(args...); },
      [](a64::Builder* as, auto... args) { as->ldrsh(args...); });
}

void translateUnreachable(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  as->udf(0);
}

void translateNegate(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const lir::Operand* opnd0 = instr->getInput(0);

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

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const lir::Operand* opnd0 = instr->getInput(0);

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

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const lir::Operand* opnd0 = instr->getInput(0);
  const lir::Operand* opnd1 = instr->getInput(1);

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

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const lir::Operand* opnd0 = instr->getInput(0);
  const lir::Operand* opnd1 = instr->getInput(1);

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

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);
  const lir::Operand* opnd0 = instr->getInput(0);
  const lir::Operand* opnd1 = instr->getInput(1);

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

  const lir::Operand* output =
      instr->getNumOutputs() > 0 ? instr->output() : instr->getInput(0);

  // Division instructions may have an extra leading Imm{0} input (used by x86
  // for the high half of the dividend). Skip it on AArch64.
  size_t base = 0;
  if (instr->getNumInputs() == 3 && instr->getInput(0)->isImm()) {
    base = 1;
  }
  const lir::Operand* opnd0 = instr->getInput(base);
  const lir::Operand* opnd1 = instr->getInput(base + 1);

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

  const lir::Operand* operand = instr->getInput(0);

  if (operand->isReg()) {
    auto reg = AT::getGpWiden(operand);
    as->str(reg, a64::ptr_pre(a64::sp, -16));
  } else if (operand->isStack()) {
    // Resolve the source slot before SP moves.
    auto ptr =
        getStackSlotPtr(env, operand->getStackSlot().loc, arch::reg_scratch_1);
    as->ldr(arch::reg_scratch_0, ptr);
    as->str(arch::reg_scratch_0, a64::ptr_pre(a64::sp, -16));
  } else {
    JIT_ABORT("Unsupported operand type for push: {}", operand->type());
  }

  env->adjustSp(16);
}

void translatePop(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const lir::Operand* operand = instr->output();

  // SP is released by the load below, so the destination slot has to be
  // resolved against the post-pop position.
  env->adjustSp(-16);

  if (operand->isReg()) {
    auto reg = AT::getGpWiden(operand);
    as->ldr(reg, a64::ptr_post(a64::sp, 16));
  } else if (operand->isStack()) {
    as->ldr(arch::reg_scratch_0, a64::ptr_post(a64::sp, 16));
    as->str(
        arch::reg_scratch_0,
        getStackSlotPtr(env, operand->getStackSlot().loc, arch::reg_scratch_1));
  } else {
    JIT_ABORT("Unsupported operand type for pop: {}", operand->type());
  }
}

void translateExchange(Environ* env, const Instruction* instr) {
  a64::Builder* as = env->as;

  const lir::Operand* opnd0 = instr->output();
  const lir::Operand* opnd1 = instr->getInput(0);

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

  const lir::Operand* inp0 = instr->getInput(0);
  const lir::Operand* inp1 = instr->getInput(1);

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

void translateBranchBit(Environ* env, const Instruction* instr, bool is_set) {
  a64::Builder* as = env->as;

  auto test_reg = AT::getGpWiden(instr->getInput(0));
  auto bit_pos = instr->getInput(1)->getConstant();
  auto label = getLabel(env, instr->getInput(2));

  if (is_set) {
    as->tbnz(test_reg, bit_pos, label);
  } else {
    as->tbz(test_reg, bit_pos, label);
  }
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
  if (data_type == jit::lir::Operand::k8bit) {
    shift = 24;
  } else if (data_type == jit::lir::Operand::k16bit) {
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
    case jit::lir::Operand::k8bit:
    case jit::lir::Operand::k16bit:
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

// Translates a single LIR instruction to machine code.
void AutoTranslator::translateInstr(Environ* env, const Instruction* instr)
    const {
  auto opcode = instr->opcode();

#if defined(CINDER_AARCH64)
  // Addressing frame slots through SP is only valid while SP and FP both hold
  // their frame positions, so writing either one gives up on it. Generators
  // are the reason this is not just a prologue/epilogue concern: they re-point
  // FP at the heap-allocated GenDataFooter partway through the function.
  // Translators that re-establish a known position (kSetupFrame, kPush, kPop)
  // do so after this runs.
  if (instr->getNumOutputs() > 0) {
    const lir::Operand* out = instr->output();
    if (out->isReg()) {
      auto loc = out->getPhyRegister();
      if (loc == SP || loc == arch::reg_frame_pointer_loc) {
        env->sp_to_fp_delta = arch::kSpPositionUnknown;
      }
    }
  }
#endif

  // Every conditional branch reads its condition out of the status flags and
  // jumps to the label in its first input, so they all lower the same way.
  if (opcode == Opcode::kBranchCC) {
    emitBranchCC(
        env->as, instr->condition(), getLabel(env, instr->getInput(0)));
    return;
  }

  switch (opcode) {
    case Opcode::kBind:
    case Opcode::kCallSiteLiveValues:
      return;
#if defined(CINDER_X86_64)
    case Opcode::kLea: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (input->isLabel()) {
        translateLeaLabel(env, instr);
      } else {
        env->as->lea(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Opcode::kMoveRelaxed: {
      checkMoveRelaxedOperandShape(instr);

      auto* output = instr->output();
      auto* input = instr->getInput(0);

      if (output->isReg()) {
        int access_size_in_bytes = getOperandSizeInBytes(instr, output);
        if (!kCinderJitTsanEnabled ||
            !tryEmitTsanRelaxedAtomicRead(
                *env, output, input, access_size_in_bytes)) {
          env->as->mov(getReg(instr, output), getMem(instr, input));
        }
      } else if (input->isReg()) {
        int access_size_in_bytes = getOperandSizeInBytes(instr, output);
        if (!kCinderJitTsanEnabled ||
            !tryEmitTsanRelaxedAtomicWrite(
                *env, output, input, access_size_in_bytes)) {
          env->as->mov(getMem(instr, output), getReg(instr, input));
        }
      } else {
        int access_size_in_bytes = getOperandSizeInBytes(instr, output);
        if (!kCinderJitTsanEnabled ||
            !tryEmitTsanRelaxedAtomicWrite(
                *env, output, input, access_size_in_bytes)) {
          env->as->mov(getMem(instr, output), getImm(input));
        }
      }
      return;
    }
    case Opcode::kZext: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      // x86-64 has no `movzx r64, r/m32`; writing a 32-bit register already
      // zeroes the upper half of its 64-bit counterpart, so a plain MOV between
      // the 32-bit halves is the zero-extend.  This stays a MOV even when the
      // source and destination registers are the same, as the upper half is not
      // known to be clear.
      if (input->sizeInBits() == 32) {
        JIT_THROW_IF(
            output->sizeInBits() != 64,
            "Zero-extend from 32-bits should always go to 64-bits, got '{}' "
            "instead",
            *instr);

        auto output_reg = asmjit::x86::gpd(output->getPhyRegister().loc);
        if (input->isReg()) {
          env->as->mov(output_reg, getReg(instr, input));
        } else {
          env->as->mov(output_reg, getMem(instr, input));
        }
        return;
      }

      if (input->isReg()) {
        env->as->movzx(getReg(instr, output), getReg(instr, input));
      } else {
        env->as->movzx(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Opcode::kSext: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      // x86-64 spells the 32 -> 64 bit sign-extend `movsxd`; `movsx` cannot
      // encode a 32-bit source.
      if (input->sizeInBits() == 32) {
        JIT_THROW_IF(
            output->sizeInBits() != 64,
            "Sign-extend from 32-bits should always go to 64-bits, got '{}' "
            "instead",
            *instr);

        if (input->isReg()) {
          env->as->movsxd(getReg(instr, output), getReg(instr, input));
        } else {
          env->as->movsxd(getReg(instr, output), getMem(instr, input));
        }
        return;
      }

      if (input->isReg()) {
        env->as->movsx(getReg(instr, output), getReg(instr, input));
      } else {
        env->as->movsx(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Opcode::kUnreachable:
      env->as->ud2();
      return;
    case Opcode::kDiv: {
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
    case Opcode::kDivUn: {
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
    case Opcode::kPush: {
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
    case Opcode::kPop: {
      auto* output = instr->output();

      if (output->isReg()) {
        env->as->pop(getReg(instr, output));
      } else {
        env->as->pop(getMem(instr, output));
      }
      return;
    }
    case Opcode::kX64Cdq: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cdq(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Opcode::kX64Cwd: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cwd(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Opcode::kX64Cqo: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      env->as->cqo(getReg(instr, output), getReg(instr, input));
      return;
    }
    case Opcode::kTest: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->test(getReg(instr, in0), getReg(instr, in1));
      return;
    }
    case Opcode::kBranch: {
      auto* input = instr->getInput(0);
      if (input->isInd() || input->isReg()) {
        translateBranchIndirect(env, instr);
      } else if (input->isImm()) {
        env->as->jmp(getImm(input));
      } else {
        env->as->jmp(getLabel(env, input));
      }
      return;
    }
    case Opcode::kGuard:
      translateGuard(env, instr);
      return;
    case Opcode::kDeoptPatchpoint:
      TranslateDeoptPatchpoint(env, instr);
      return;
    case Opcode::kLoadThreadState:
      translateLoadThreadState(env, instr);
      return;
    case Opcode::kStoreGenYieldPoint:
      translateStoreGenYieldPoint(env, instr);
      return;
    case Opcode::kStoreGenYieldFromPoint:
      translateStoreGenYieldFromPoint(env, instr);
      return;
    case Opcode::kBranchToYieldExit:
      JIT_ABORT("kBranchToYieldExit should have been removed by regalloc");
    case Opcode::kResumeGenYield:
      translateResumeGenYield(env, instr);
      return;
    case Opcode::kEpilogueEnd:
      translateEpilogueEnd(env, instr);
      return;
    case Opcode::kIntToBool:
      translateIntToBool(env, instr);
      return;
    case Opcode::kPrologue:
      translatePrologue(env, instr);
      return;
    case Opcode::kSetupFrame:
      translateSetupFrame(env, instr);
      return;
    case Opcode::kInc: {
      auto* input = instr->getInput(0);

      if (input->isStack()) {
        env->as->inc(getMem(instr, input));
      } else {
        env->as->inc(getReg(instr, input));
      }
      return;
    }
    case Opcode::kDec: {
      auto* input = instr->getInput(0);

      if (input->isStack()) {
        env->as->dec(getMem(instr, input));
      } else {
        env->as->dec(getReg(instr, input));
      }
      return;
    }
    case Opcode::kBranchBitSet:
    case Opcode::kBranchBitNotSet: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);
      auto label = getLabel(env, instr->getInput(2));

      env->as->bt(getReg(instr, in0), getImm(in1));
      if (instr->isBranchBitSet()) {
        env->as->jc(label);
      } else {
        env->as->jnc(label);
      }
      return;
    }
    case Opcode::kSelect: {
      auto output = getReg(instr, instr->output());
      auto condition = getReg(instr, instr->getInput(0));
      auto false_val = instr->getInput(2);

      if (false_val->isImm()) {
        env->as->mov(output, getImm(false_val));
      } else {
        env->as->mov(output, getReg(instr, false_val));
      }
      env->as->test(condition, condition);
      env->as->cmovnz(output, getReg(instr, instr->getInput(1)));
      return;
    }
    case Opcode::kCompare:
      TranslateCompare(env, instr);
      return;
    case Opcode::kFadd: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->addsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->addsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Opcode::kFsub: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->subsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->subsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Opcode::kFmul: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->mulsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->mulsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Opcode::kFdiv: {
      if (instr->getNumOutputs() > 0) {
        env->as->movsd(getVecD(instr->output()), getVecD(instr->getInput(0)));
        env->as->divsd(getVecD(instr->output()), getVecD(instr->getInput(1)));
      } else {
        env->as->divsd(
            getVecD(instr->getInput(0)), getVecD(instr->getInput(1)));
      }
      return;
    }
    case Opcode::kExchange: {
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
    case Opcode::kCmp: {
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
    case Opcode::kNegate: {
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
    case Opcode::kInvert: {
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
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kMul: {
      auto emitOp = [&](const auto& dst, const auto& src) {
        // NOLINTNEXTLINE(clang-diagnostic-switch-enum)
        switch (opcode) {
          case Opcode::kAdd:
            env->as->add(dst, src);
            break;
          case Opcode::kSub:
            env->as->sub(dst, src);
            break;
          case Opcode::kAnd:
            env->as->and_(dst, src);
            break;
          case Opcode::kOr:
            env->as->or_(dst, src);
            break;
          case Opcode::kXor:
            env->as->xor_(dst, src);
            break;
          case Opcode::kMul:
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
    case Opcode::kLShift:
    case Opcode::kRShift:
    case Opcode::kRShiftUn:
      translateShift(env, instr);
      return;
    case Opcode::kTest32: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->test(
          asmjit::x86::gpd(in0->getPhyRegister().loc),
          asmjit::x86::gpd(in1->getPhyRegister().loc));
      return;
    }
    case Opcode::kInt64ToDouble: {
      auto* input = instr->getInput(0);

      if (input->isReg()) {
        env->as->cvtsi2sd(getVecD(instr->output()), getReg(instr, input));
      } else {
        env->as->cvtsi2sd(getVecD(instr->output()), getMem(instr, input));
      }
      return;
    }
    case Opcode::kCall: {
      auto* input = instr->getInput(0);

      if (input->isImm()) {
        env->as->call(getImm(input));
      } else if (input->isLabel()) {
        env->as->call(getLabel(env, input));
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
      fillCallSiteLiveValueLocations(env, instr);
      return;
    }
    case Opcode::kMove: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);

      // Move is restricted to register-to-register and immediate-to-register.
      if (output->isReg() && output->isVecD()) {
        if (input->isReg() && input->isVecD()) {
          env->as->movsd(getVecD(output), getVecD(input));
        } else if (input->isReg()) {
          env->as->movq(getVecD(output), getReg(instr, input));
        } else {
          JIT_ABORT(
              "Move with VecD output only supports Reg inputs, got {} in {}",
              input->type(),
              *instr);
        }
      } else if (output->isReg()) {
        if (input->isReg() && input->isVecD()) {
          env->as->movq(getReg(instr, output), getVecD(input));
        } else if (input->isReg()) {
          env->as->mov(getReg(instr, output), getReg(instr, input));
        } else if (input->isImm()) {
          env->as->mov(getReg(instr, output), getImm(input));
        } else {
          JIT_ABORT(
              "Move with Reg output only supports Reg/VecD/Imm inputs, got {} "
              "(use Load for memory) in {}",
              input->type(),
              *instr);
        }
      } else {
        JIT_ABORT(
            "Move output must be a register, got {} (use Store for memory) in "
            "{}",
            output->type(),
            *instr);
      }
      return;
    }
    case Opcode::kLoad: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);
      JIT_CHECK(
          output->isReg(),
          "Load output must be a register, got {} in {}",
          output->type(),
          *instr);
      JIT_CHECK(
          isMemoryMoveOperand(input),
          "Load input must be memory (Stk/Mem/Ind), got {} in {}",
          input->type(),
          *instr);
      if (output->isVecD()) {
        if constexpr (kCinderJitTsanEnabled) {
          int access_size_in_bytes = getOperandSizeInBytes(instr, output);
          emitTsanRead(*env, input, access_size_in_bytes);
        }
        env->as->movsd(getVecD(output), getMem(instr, input));
      } else {
        if constexpr (kCinderJitTsanEnabled) {
          int access_size_in_bytes = getOperandSizeInBytes(instr, output);
          emitTsanRead(*env, input, access_size_in_bytes);
        }
        env->as->mov(getReg(instr, output), getMem(instr, input));
      }
      return;
    }
    case Opcode::kStore: {
      auto* output = instr->output();
      auto* input = instr->getInput(0);
      JIT_CHECK(
          isMemoryMoveOperand(output),
          "Store output must be memory (Stk/Mem/Ind), got {}",
          output->type());
      if constexpr (kCinderJitTsanEnabled) {
        int access_size_in_bytes = getOperandSizeInBytes(instr, output);
        emitTsanWrite(*env, output, access_size_in_bytes);
      }
      if (input->isReg() && input->isVecD()) {
        env->as->movsd(getMem(instr, output), getVecD(input));
      } else if (input->isReg()) {
        env->as->mov(getMem(instr, output), getReg(instr, input));
      } else if (input->isImm()) {
        env->as->mov(getMem(instr, output), getImm(input));
      } else {
        JIT_ABORT("Store input must be Reg or Imm, got {}", input->type());
      }
      return;
    }
    case Opcode::kReserveStack:
      translateReserveStack(env, instr);
      return;
    case Opcode::kVariadicPush:
      translateVariadicPush(env, instr);
      return;
    case Opcode::kStorePair:
      translateStorePair(env, instr);
      return;
    case Opcode::kLoadPair:
      translateLoadPair(env, instr);
      return;
    case Opcode::kLeave:
      translateLeave(env);
      return;
    case Opcode::kRet:
      translateRet(env);
      return;
    case Opcode::kNop:
    case Opcode::kVectorCallTstate:
    case Opcode::kVarArgCall:
    case Opcode::kMulAdd:
    case Opcode::kLoadArg:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kMovConstPool:
    case Opcode::kCmpBranchZero:
    case Opcode::kCmpBranchNonZero:
    case Opcode::kCondBranch:
    case Opcode::kPhi:
    case Opcode::kReturn:
      JIT_ABORT("Unexpected opcode {} in translateInstr", (int)opcode);
#elif defined(CINDER_AARCH64)
    case Opcode::kLea: {
      auto* input = instr->getInput(0);

      if (input->isLabel()) {
        translateLeaLabel(env, instr);
      } else {
        translateLea(env, instr);
      }
      return;
    }
    case Opcode::kMoveRelaxed:
      translateMove(env, instr);
      return;
    case Opcode::kZext:
      translateZext(env, instr);
      return;
    case Opcode::kSext:
      translateSext(env, instr);
      return;
    case Opcode::kUnreachable:
      translateUnreachable(env, instr);
      return;
    case Opcode::kDiv:
      translateDiv(env, instr);
      return;
    case Opcode::kDivUn:
      translateDivUn(env, instr);
      return;
    case Opcode::kPush:
      translatePush(env, instr);
      return;
    case Opcode::kPop:
      translatePop(env, instr);
      return;
    case Opcode::kTest:
      translateTst(env, instr);
      return;
    case Opcode::kBranch: {
      auto* input = instr->getInput(0);
      if (input->isInd() || input->isReg()) {
        translateBranchIndirect(env, instr);
      } else if (input->isImm()) {
        env->as->b(static_cast<uint64_t>(input->getConstant()));
      } else {
        env->as->b(getLabel(env, input));
      }
      return;
    }
    case Opcode::kCmpBranchZero:
      env->as->cbz(
          getGpWiden(instr->getInput(0)), getLabel(env, instr->getInput(1)));
      return;
    case Opcode::kCmpBranchNonZero:
      env->as->cbnz(
          getGpWiden(instr->getInput(0)), getLabel(env, instr->getInput(1)));
      return;
    case Opcode::kA64GuardCC:
      translateA64GuardCC(env, instr);
      return;
    case Opcode::kGuard:
      translateGuard(env, instr);
      return;
    case Opcode::kDeoptPatchpoint:
      TranslateDeoptPatchpoint(env, instr);
      return;
    case Opcode::kLoadThreadState:
      translateLoadThreadState(env, instr);
      return;
    case Opcode::kStoreGenYieldPoint:
      translateStoreGenYieldPoint(env, instr);
      return;
    case Opcode::kStoreGenYieldFromPoint:
      translateStoreGenYieldFromPoint(env, instr);
      return;
    case Opcode::kBranchToYieldExit:
      JIT_ABORT("kBranchToYieldExit should have been removed by regalloc");
    case Opcode::kResumeGenYield:
      translateResumeGenYield(env, instr);
      return;
    case Opcode::kEpilogueEnd:
      translateEpilogueEnd(env, instr);
      return;
    case Opcode::kIntToBool:
      translateIntToBool(env, instr);
      return;
    case Opcode::kPrologue:
      translatePrologue(env, instr);
      return;
    case Opcode::kSetupFrame:
      translateSetupFrame(env, instr);
      return;
    case Opcode::kInc:
      translateInc(env, instr);
      return;
    case Opcode::kDec:
      translateDec(env, instr);
      return;
    case Opcode::kBranchBitSet:
      translateBranchBit(env, instr, true);
      return;
    case Opcode::kBranchBitNotSet:
      translateBranchBit(env, instr, false);
      return;
    case Opcode::kSelect:
      translateSelect(env, instr);
      return;
    case Opcode::kCompare:
      TranslateCompare(env, instr);
      return;
    case Opcode::kFadd: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fadd(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fadd(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Opcode::kFsub: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fsub(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fsub(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Opcode::kFmul: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fmul(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fmul(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Opcode::kFdiv: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      if (instr->getNumOutputs() > 0) {
        env->as->fdiv(getVecD(instr->output()), getVecD(in0), getVecD(in1));
      } else {
        env->as->fdiv(getVecD(in0), getVecD(in0), getVecD(in1));
      }
      return;
    }
    case Opcode::kInt64ToDouble:
      env->as->scvtf(
          getVecD(instr->output()), getReg(instr, instr->getInput(0)));
      return;
    case Opcode::kExchange:
      translateExchange(env, instr);
      return;
    case Opcode::kCmp:
      translateCmp(env, instr);
      return;
    case Opcode::kNegate:
      translateNegate(env, instr);
      return;
    case Opcode::kInvert:
      translateInvert(env, instr);
      return;
    case Opcode::kAdd:
      translateAdd(env, instr);
      return;
    case Opcode::kSub:
      translateSub(env, instr);
      return;
    case Opcode::kAnd:
      translateAnd(env, instr);
      return;
    case Opcode::kOr:
      translateOr(env, instr);
      return;
    case Opcode::kXor:
      translateXor(env, instr);
      return;
    case Opcode::kMul:
      translateMul(env, instr);
      return;
    case Opcode::kLShift:
    case Opcode::kRShift:
    case Opcode::kRShiftUn:
      translateShift(env, instr);
      return;
    case Opcode::kTest32: {
      auto* in0 = instr->getInput(0);
      auto* in1 = instr->getInput(1);

      env->as->tst(
          asmjit::a64::w(in0->getPhyRegister().loc),
          asmjit::a64::w(in1->getPhyRegister().loc));
      return;
    }
    case Opcode::kCall:
      translateCall(env, instr);
      fillCallSiteLiveValueLocations(env, instr);
      return;
    case Opcode::kMove:
      translateMove(env, instr);
      return;
    case Opcode::kLoad:
      translateLoad(env, instr);
      return;
    case Opcode::kStore:
      translateStore(env, instr);
      return;
    case Opcode::kMovConstPool:
      translateMovConstPool(env, instr);
      return;
    case Opcode::kMulAdd:
      translateMulAdd(env, instr);
      return;
    case Opcode::kReserveStack:
      translateReserveStack(env, instr);
      return;
    case Opcode::kVariadicPush:
      translateVariadicPush(env, instr);
      return;
    case Opcode::kStorePair:
      translateStorePair(env, instr);
      return;
    case Opcode::kLoadPair:
      translateLoadPair(env, instr);
      return;
    case Opcode::kLeave:
      translateLeave(env);
      return;
    case Opcode::kRet:
      translateRet(env);
      return;
    case Opcode::kNop:
    case Opcode::kVectorCallTstate:
    case Opcode::kVarArgCall:
    case Opcode::kLoadArg:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kCondBranch:
    case Opcode::kPhi:
    case Opcode::kReturn:
      JIT_ABORT(
          "Unexpected opcode {} ({}) in translateInstr",
          opname(opcode),
          static_cast<int>(opcode));
#endif
    default:
      JIT_ABORT(
          "No handler for opcode {} ({})",
          opname(opcode),
          static_cast<int>(opcode));
  }
}

} // namespace cinderx::jit::codegen::autogen
