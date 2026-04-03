// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/autogen.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/gen_asm_utils.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/printer.h"

#include <type_traits>
#include <vector>

using namespace asmjit;
using namespace jit::lir;
using namespace jit::codegen;

namespace jit::codegen::autogen {

#define ANY "*"

namespace {
// Add a pattern to an existing trie tree. If the trie tree is nullptr, create a
// new one.
std::unique_ptr<PatternNode> addPattern(
    std::unique_ptr<PatternNode> patterns,
    const std::string& s,
    PatternNode::func_t func) {
  JIT_DCHECK(!s.empty(), "pattern string should not be empty.");

  if (patterns == nullptr) {
    patterns = std::make_unique<PatternNode>();
  }

  PatternNode* cur = patterns.get();
  for (auto& c : s) {
    auto iter = cur->next.find(c);
    if (iter == cur->next.end()) {
      cur = cur->next.emplace(c, std::make_unique<PatternNode>())
                .first->second.get();
      continue;
    }
    cur = iter->second.get();
  }

  JIT_DCHECK(cur->func == nullptr, "Found duplicated pattern.");
  cur->func = func;

  return patterns;
}

// Find the function associated to the pattern given in s.
PatternNode::func_t findByPattern(
    const PatternNode* patterns,
    const std::string& s) {
  auto cur = patterns;
  if (s.empty()) {
    // handle the special case of matching '*' with an empty string
    auto iter = cur->next.find('*');
    if (iter != cur->next.end()) {
      cur = iter->second.get();
      return cur->func;
    }
  }
  for (auto& c : s) {
    auto iter = cur->next.find(c);
    if (iter != cur->next.end()) {
      cur = iter->second.get();
      continue;
    }

    iter = cur->next.find('?');
    if (iter != cur->next.end()) {
      cur = iter->second.get();
      continue;
    }

    iter = cur->next.find('*');
    if (iter != cur->next.end()) {
      cur = iter->second.get();
      break;
    }

    return nullptr;
  }

  return cur->func;
}

} // namespace

// this function generates operand patterns from the inputs and outputs
// of a given instruction instr and calls the correspoinding code generation
// functions.
void AutoTranslator::translateInstr(Environ* env, const Instruction* instr)
    const {
  auto opcode = instr->opcode();
  if (opcode == Instruction::kBind) {
    return;
  }
  auto& instr_map = map_get(instr_rule_map_, opcode);

  std::string pattern;
  pattern.reserve(instr->getNumInputs() + instr->getNumOutputs());

  if (instr->getNumOutputs()) {
    auto operand = instr->output();

    switch (operand->type()) {
      case OperandBase::kReg:
        pattern += (operand->isVecD() ? "X" : "R");
        break;
      case OperandBase::kStack:
      case OperandBase::kMem:
      case OperandBase::kInd:
        pattern += "M";
        break;
      default:
        JIT_ABORT("Output operand has to be of type register or memory");
    }
  }

  instr->foreachInputOperand([&](const OperandBase* operand) {
    switch (operand->type()) {
      case OperandBase::kReg:
        pattern += (operand->isVecD() ? "x" : "r");
        break;
      case OperandBase::kStack:
      case OperandBase::kMem:
      case OperandBase::kInd:
        pattern += "m";
        break;
      case OperandBase::kImm:
        pattern += "i";
        break;
      case OperandBase::kLabel:
        pattern += "b";
        break;
      default:
        JIT_ABORT(
            "Illegal input type {} for instruction {}",
            operand->type(),
            *instr);
    }
  });

  auto func = findByPattern(instr_map.get(), pattern);
  JIT_CHECK(
      func != nullptr,
      "No pattern found for opcode {}: {}",
      InstrProperty::getProperties(instr).name,
      pattern);
  func(env, instr);
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
    case kNotNegative: {
      // Ideally we'd do but we don't know if we're outside the 32kb
      // displacement limit as->tbnz(reg, sign_bit, deopt_label);
      auto skip = as->newLabel();
      as->tbz(reg, sign_bit, skip);
      as->b(deopt_label);
      as->bind(skip);
      break;
    }
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

// ***********************************************************************
// The following templates and macros implement the auto generation table.
// The generator table defines a hash table, whose key is instruction type,
// and value is another hash table mapping instruction operand pattern and
// a function carrying out certain Actions for the instruction with the
// operand pattern.
// The list of Actions are encoded in the template class RuleActions as its
// template arguments. Currently, there are two types of Actions:
//   * AsmAction - generate an asm instruction
//   * CallAction - call a user defined instruction
// The Action classes are also templates, whose argument lists encode the
// parameters for the Action. For example, an AsmAction's argument list has
// the assembly instruction mnemonic and its operands.
// ***********************************************************************
template <int N>
const OperandBase* LIROperandMapper(const Instruction* instr) {
  auto num_outputs = instr->getNumOutputs();
  if (N < num_outputs) {
    return instr->output();
  } else {
    return instr->getInput(N - num_outputs);
  }
}

template <int N>
int LIROperandSizeMapper(const Instruction* instr) {
  auto size_type = InstrProperty::getProperties(instr).opnd_size_type;
  switch (size_type) {
    case kDefault:
      return LIROperandMapper<N>(instr)->sizeInBits();
    case kAlways64:
      return 64;
    case kOut:
      return LIROperandMapper<0>(instr)->sizeInBits();
  }

  JIT_ABORT("Unknown size type");
}

template <int N>
struct ImmOperand {
  using asmjit_type = const asmjit::Imm&;

  static asmjit::Imm GetAsmOperand(Environ*, const Instruction* instr) {
    return asmjit::Imm(LIROperandMapper<N>(instr)->getConstant());
  }
};

template <typename T>
struct ImmOperandNegate {
  using asmjit_type = const asmjit::Imm&;

  static asmjit::Imm GetAsmOperand(Environ* env, const Instruction* instr) {
    return asmjit::Imm(
        -T::GetAsmOperand(env, instr).template valueAs<int64_t>());
  }
};

template <typename T>
struct ImmOperandInvert {
  using asmjit_type = const asmjit::Imm&;

  static asmjit::Imm GetAsmOperand(Environ* env, const Instruction* instr) {
    return asmjit::Imm(
        ~T::GetAsmOperand(env, instr).template valueAs<uint64_t>());
  }
};

template <int N, int Size = -1>
struct RegOperand {
  using asmjit_type = const arch::Gp&;
  static arch::Gp GetAsmOperand(Environ*, const Instruction* instr) {
    static_assert(
        Size == -1 || Size == 8 || Size == 16 || Size == 32 || Size == 64,
        "Invalid Size");

#if defined(CINDER_X86_64)
    int size = Size == -1 ? LIROperandSizeMapper<N>(instr) : Size;

    PhyLocation reg = LIROperandMapper<N>(instr)->getPhyRegister();
    switch (size) {
      case 8:
        return asmjit::x86::gpb(reg.loc);
      case 16:
        return asmjit::x86::gpw(reg.loc);
      case 32:
        return asmjit::x86::gpd(reg.loc);
      case 64:
        return asmjit::x86::gpq(reg.loc);
    }
#elif defined(CINDER_AARCH64)
    int size = Size == -1 ? LIROperandSizeMapper<N>(instr) : Size;

    PhyLocation reg = LIROperandMapper<N>(instr)->getPhyRegister();
    switch (size) {
      case 8:
      case 16:
        JIT_ABORT("Currently unsupported size.");
      case 32:
        return asmjit::a64::w(reg.loc);
      case 64:
        return asmjit::a64::x(reg.loc);
    }
#else
    CINDER_UNSUPPORTED
#endif

    JIT_ABORT("Incorrect operand size.");
  }
};

template <int N>
struct VecDOperand {
  using asmjit_type = const arch::VecD&;
  static arch::VecD GetAsmOperand(Environ*, const Instruction* instr) {
#if defined(CINDER_X86_64)
    return asmjit::x86::xmm(
        LIROperandMapper<N>(instr)->getPhyRegister().loc - VECD_REG_BASE);
#elif defined(CINDER_AARCH64)
    return asmjit::a64::d(
        LIROperandMapper<N>(instr)->getPhyRegister().loc - VECD_REG_BASE);
#else
    CINDER_UNSUPPORTED
    return arch::VecD();
#endif
  }
};

#define OP(v)                                       \
  typename std::conditional_t<                      \
      pattern[v] == 'i',                            \
      ImmOperand<v>,                                \
      std::conditional_t<                           \
          (pattern[v] == 'x' || pattern[v] == 'X'), \
          VecDOperand<v>,                           \
          RegOperand<v>>>

#define REG_OP(v, size) RegOperand<v, size>

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

template <int N>
struct MemOperand {
  using asmjit_type = const arch::Mem&;
  static arch::Mem GetAsmOperand(Environ*, const Instruction* instr) {
#if defined(CINDER_X86_64)
    const OperandBase* operand = LIROperandMapper<N>(instr);
    auto size = LIROperandSizeMapper<N>(instr) / 8;

    asmjit::x86::Mem memptr;
    if (operand->isStack()) {
      memptr = asmjit::x86::ptr(asmjit::x86::rbp, operand->getStackSlot().loc);
    } else if (operand->isMem()) {
      memptr = asmjit::x86::ptr(
          reinterpret_cast<uint64_t>(operand->getMemoryAddress()));
    } else if (operand->isInd()) {
      memptr = AsmIndirectOperandBuilder(operand);
    } else {
      JIT_ABORT("Unsupported operand type.");
    }

    memptr.setSize(size);
    return memptr;
#elif defined(CINDER_AARCH64)
    const OperandBase* operand = LIROperandMapper<N>(instr);
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
};

#define MEM(m) MemOperand<m>
#define STK(v) MemOperand<v>

template <int N>
struct LabelOperand {
  using asmjit_type = const asmjit::Label&;
  static asmjit::Label GetAsmOperand(Environ* env, const Instruction* instr) {
    auto* operand = LIROperandMapper<N>(instr);
    if (operand->getDefine()->hasAsmLabel()) {
      return operand->getDefine()->getAsmLabel();
    }
    auto block = operand->getBasicBlock();
    return map_get(env->block_label_map, block);
  }
};

#define LBL(v) LabelOperand<v>

template <typename... Args>
struct OperandList;

template <typename FuncType, FuncType func, typename OpndList>
struct AsmAction;

template <typename FuncType, FuncType func, typename... OpndTypes>
struct AsmAction<FuncType, func, OperandList<OpndTypes...>> {
  static void eval(Environ* env, const Instruction* instr) {
    static_cast<void>(instr);
    (env->as->*func)(OpndTypes::GetAsmOperand(env, instr)...);
  }
};

template <typename... Args>
struct AsminstructionType {
  using type = asmjit::Error (arch::EmitterExplicitT<arch::Builder>::*)(
      typename Args::asmjit_type...);
};

template <void (*func)(Environ*, const Instruction*)>
struct CallAction {
  static void eval(Environ* env, const Instruction* instr) {
    func(env, instr);
  }
};

template <typename... Actions>
struct RuleActions;

template <typename AAction, typename... Actions>
struct RuleActions<AAction, Actions...> {
  static void eval(Environ* env, const Instruction* instr) {
    AAction::eval(env, instr);
    RuleActions<Actions...>::eval(env, instr);
  }
};

template <>
struct RuleActions<> {
  static void eval(Environ*, const Instruction*) {}
};

struct AddDebugEntryAction {
  static void eval(Environ* env, const Instruction* instr) {
    asmjit::Label label = env->as->newLabel();
    env->as->bind(label);
    if (instr->origin()) {
      env->pending_debug_locs.emplace_back(label, instr->origin());
    }
  }
};

void translateYieldExitPoint(Environ* env, const Instruction*) {
  env->as->bind(env->exit_for_yield_label);
}

#if defined(CINDER_AARCH64)
void restoreCalleeSavedRegsAarch64(
    arch::Builder* as,
    PhyRegisterSet saved_regs) {
  auto gp_regs = saved_regs & ALL_GP_REGISTERS;
  auto vecd_regs = saved_regs & ALL_VECD_REGISTERS;

  // Restore VecD registers first (they were saved last, so they're at the
  // lowest addresses).
  if (!vecd_regs.Empty()) {
    bool odd = vecd_regs.count() % 2 == 1;
    PhyRegisterSet vecd_pairs = vecd_regs;
    if (odd) {
      vecd_pairs.RemoveFirst();
    }
    while (!vecd_pairs.Empty()) {
      auto second = a64::d(vecd_pairs.GetLast().loc - VECD_REG_BASE);
      vecd_pairs.RemoveLast();
      auto first = a64::d(vecd_pairs.GetLast().loc - VECD_REG_BASE);
      vecd_pairs.RemoveLast();
      as->ldp(first, second, a64::ptr_post(a64::sp, 16));
    }
    if (odd) {
      as->ldr(
          a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
          a64::ptr_post(a64::sp, 16));
    }
  }

  // Restore GP registers (they were saved first, so they're at higher
  // addresses).
  if (!gp_regs.Empty()) {
    bool odd = gp_regs.count() % 2 == 1;
    PhyRegisterSet gp_pairs = gp_regs;
    if (odd) {
      gp_pairs.RemoveFirst();
    }
    while (!gp_pairs.Empty()) {
      auto second = a64::x(gp_pairs.GetLast().loc);
      gp_pairs.RemoveLast();
      auto first = a64::x(gp_pairs.GetLast().loc);
      gp_pairs.RemoveLast();
      as->ldp(first, second, a64::ptr_post(a64::sp, 16));
    }
    if (odd) {
      as->ldr(a64::x(gp_regs.GetFirst().loc), a64::ptr_post(a64::sp, 16));
    }
  }
}
#endif

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
    arch::add_signed_immediate(
        as, a64::sp, arch::fp, -env->last_callee_saved_reg_off);
    restoreCalleeSavedRegsAarch64(as, saved_regs);
  }
  as->mov(a64::sp, arch::fp);
  as->ldp(arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
  as->ret(arch::lr);
#else
  CINDER_UNSUPPORTED
#endif
}

} // namespace

#define ASM(instr, args...)                    \
  AsmAction<                                   \
      typename AsminstructionType<args>::type, \
      &arch::Builder::instr,                   \
      OperandList<args>>

// Can't be named CALL as that conflicts with the opcode.
#define CALL_C(func) CallAction<func>

#define ADDDEBUGENTRY() AddDebugEntryAction

#define BEGIN_RULE_TABLE void AutoTranslator::initTable() {
#define END_RULE_TABLE }

#define BEGIN_RULES(__t)                                \
  {                                                     \
    auto& __rules = instr_rule_map_                     \
                        .emplace(                       \
                            std::piecewise_construct,   \
                            std::forward_as_tuple(__t), \
                            std::forward_as_tuple())    \
                        .first->second;

#define END_RULES }
#define GEN(s, actions...)                                  \
  {                                                         \
    UNUSED constexpr char pattern[] = s;                    \
    using rule_actions = RuleActions<actions>;              \
    auto gen = [](Environ* env, const Instruction* instr) { \
      rule_actions::eval(env, instr);                       \
    };                                                      \
    __rules = addPattern(std::move(__rules), s, gen);       \
  }

// ***********************************************************************
// Definition of Auto Generation Table
// The table consisting of multiple rules, and the rules for the same LIR
// instruction are grouped by BEGIN_RULES(LIR instruction type) and
// END_RULES.
// GEN defines a rule for a certain operand pattern of the LIR instruction,
// and maps it to a list of actions:
//   GEN(<operand pattern>, action1, action2, ...)
//
// The operand pattern is defined by a string, and each character in the string
// correpsonds to an operand of the instruction. The character can be one
// of the following:
//   * 'R' - general purpose register operand output
//   * 'r' - general purpose register operand input
//   * 'X' - floating-point register operand output
//   * 'x' - floating-point register operand input
//   * 'i' - immediate operand input
//   * 'M' - memory stack operand output
//   * 'm' - memory stack operand input
// Wildcards "?" and "*" can also be used in patterns, where "?" represents any
// one of the types listed above and "*" represents one or more above types.
// Please note that while "?" can appear anywhere in a pattern, "*" can only be
// used at the end of a pattern.
// The actions can be ASM and CALL_C, meaning generating an assembly instruction
// and call a user-defined function, respectively. The first argument of ASM
// action is the mnemonic of the instruction to be generated, and the following
// arguments are the operands to the instruction. Currently, we have four types
// of assembly instruction operands:
//   * OP  - either an immediate operand or register oeprand
//   * STK - a memory stack location [RBP - ?]
//   * LBL - a label to a basic block
//   * MEM - a memory operand. The size of the memory operand will be set to the
//           size of the LIR instruction operand specified by the first argument
//           of MEM.
// The assembly instruction operands are constructed from one or more LIR
// instruction operands. To specify the LIR operands, we use indices
// of the pattern string. For example:
//   GEN("Rri", ASM(mov, OP(0), MEM(0, 1, 2)))
// means generating a mov instruction, whose first operand is a
// register/immediate operand, constructed from the only output of the LIR
// instruction, and the second operand is memory operand, constructed from the
// register input and the immediate input of the LIR instruction. The size of
// the memory operand is set to the size of the output of the LIR instruction.
// ***********************************************************************

#if defined(CINDER_X86_64)
// clang-format off
BEGIN_RULE_TABLE

BEGIN_RULES(Instruction::kLea)
  GEN("Rm", ASM(lea, OP(0), MEM(1)))
  GEN("Rb", CALL_C(translateLeaLabel))
END_RULES

BEGIN_RULES(Instruction::kCall)
  GEN("Ri", ASM(call, OP(1)), ADDDEBUGENTRY())
  GEN("Rr", ASM(call, OP(1)), ADDDEBUGENTRY())
  GEN("i", ASM(call, OP(0)), ADDDEBUGENTRY())
  GEN("r", ASM(call, OP(0)), ADDDEBUGENTRY())
  GEN("m", ASM(call, STK(0)), ADDDEBUGENTRY())
END_RULES

BEGIN_RULES(Instruction::kMove)
  GEN("Rr", ASM(mov, OP(0), OP(1)))
  GEN("Ri", ASM(mov, OP(0), OP(1)))
  GEN("Rm", ASM(mov, OP(0), MEM(1)))
  GEN("Mr", ASM(mov, MEM(0), OP(1)))
  GEN("Mi", ASM(mov, MEM(0), OP(1)))
  GEN("Xx", ASM(movsd, OP(0), OP(1)))
  GEN("Xm", ASM(movsd, OP(0), MEM(1)))
  GEN("Mx", ASM(movsd, MEM(0), OP(1)))
  GEN("Xr", ASM(movq, OP(0), OP(1)))
  GEN("Rx", ASM(movq, OP(0), OP(1)))
END_RULES

// Atomic move with relaxed ordering.
// On x86-64, relaxed loads/stores are plain mov.
// This corresponds to the C++/C memory_order_relaxed.
BEGIN_RULES(Instruction::kMoveRelaxed)
  GEN("Rm", ASM(mov, OP(0), MEM(1)))
  GEN("Mr", ASM(mov, MEM(0), OP(1)))
  GEN("Mi", ASM(mov, MEM(0), OP(1)))
END_RULES


BEGIN_RULES(Instruction::kGuard)
  GEN(ANY, CALL_C(TranslateGuard));
END_RULES

BEGIN_RULES(Instruction::kDeoptPatchpoint)
  GEN(ANY, CALL_C(TranslateDeoptPatchpoint));
END_RULES

BEGIN_RULES(Instruction::kNegate)
  GEN("r", ASM(neg, OP(0)))
  GEN("Ri", ASM(mov, OP(0), ImmOperandNegate<OP(1)>))
  GEN("Rr", ASM(mov, OP(0), OP(1)), ASM(neg, OP(0)))
  GEN("Rm", ASM(mov, OP(0), STK(1)), ASM(neg, OP(0)))
END_RULES

BEGIN_RULES(Instruction::kInvert)
  GEN("Ri", ASM(mov, OP(0), ImmOperandInvert<OP(1)>))
  GEN("Rr", ASM(mov, OP(0), OP(1)), ASM(not_, OP(0)))
  GEN("Rm", ASM(mov, OP(0), STK(1)), ASM(not_, OP(0)))
END_RULES

BEGIN_RULES(Instruction::kMovZX)
  GEN("Rr", ASM(movzx, OP(0), OP(1)))
  GEN("Rm", ASM(movzx, OP(0), STK(1)))
END_RULES

BEGIN_RULES(Instruction::kMovSX)
  GEN("Rr", ASM(movsx, OP(0), OP(1)))
  GEN("Rm", ASM(movsx, OP(0), STK(1)))
END_RULES

BEGIN_RULES(Instruction::kMovSXD)
  GEN("Rr", ASM(movsxd, OP(0), OP(1)))
  GEN("Rm", ASM(movsxd, OP(0), STK(1)))
END_RULES

BEGIN_RULES(Instruction::kUnreachable)
  GEN(ANY, ASM(ud2))
END_RULES

#define DEF_BINARY_OP_RULES(name, instr) \
  BEGIN_RULES(Instruction::name) \
    GEN("ri", ASM(instr, OP(0), OP(1))) \
    GEN("rr", ASM(instr, OP(0), OP(1))) \
    GEN("rm", ASM(instr, OP(0), STK(1))) \
    /* rewriteBinaryOpInstrs() makes it safe to write the output before reading
     * all inputs without inputs_live_across being set for most binary ops; see
     * postalloc.cpp for details. */ \
    GEN("Rri", ASM(mov, OP(0), OP(1)), ASM(instr, OP(0), OP(2))) \
    GEN("Rrr", ASM(mov, OP(0), OP(1)), ASM(instr, OP(0), OP(2))) \
    GEN("Rrm", ASM(mov, OP(0), OP(1)), ASM(instr, OP(0), STK(2))) \
  END_RULES

DEF_BINARY_OP_RULES(kAdd, add)
DEF_BINARY_OP_RULES(kSub, sub)
DEF_BINARY_OP_RULES(kAnd, and_)
DEF_BINARY_OP_RULES(kOr, or_)
DEF_BINARY_OP_RULES(kXor, xor_)
DEF_BINARY_OP_RULES(kMul, imul)

BEGIN_RULES(Instruction::kDiv)
  GEN("rrr", ASM(idiv, OP(0), OP(1), OP(2)) )
  GEN("rrm", ASM(idiv, OP(0), OP(1), STK(2)) )
  GEN("rr", ASM(idiv, OP(0), OP(1)) )
  GEN("rm", ASM(idiv, OP(0), STK(1)) )
END_RULES

BEGIN_RULES(Instruction::kDivUn)
  GEN("rrr", ASM(div, OP(0), OP(1), OP(2)) )
  GEN("rrm", ASM(div, OP(0), OP(1), STK(2)) )
  GEN("rr", ASM(div, OP(0), OP(1)) )
  GEN("rm", ASM(div, OP(0), STK(1)) )
END_RULES

#undef DEF_BINARY_OP_RULES

BEGIN_RULES(Instruction::kFadd)
  /* rewriteBinaryOpInstrs() makes it safe to write the output before reading
   * all inputs without inputs_live_across being set for Fadd; see
   * postalloc.cpp for details. */
  GEN("Xxx", ASM(movsd, OP(0), OP(1)), ASM(addsd, OP(0), OP(2)))
  GEN("xx", ASM(addsd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFsub)
  GEN("Xxx", ASM(movsd, OP(0), OP(1)), ASM(subsd, OP(0), OP(2)))
  GEN("xx", ASM(subsd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFmul)
  /* rewriteBinaryOpInstrs() makes it safe to write the output before reading
   * all inputs without inputs_live_across being set for Fmul; see
   * postalloc.cpp for details. */
  GEN("Xxx", ASM(movsd, OP(0), OP(1)), ASM(mulsd, OP(0), OP(2)))
  GEN("xx", ASM(mulsd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFdiv)
  GEN("Xxx", ASM(movsd, OP(0), OP(1)), ASM(divsd, OP(0), OP(2)))
  GEN("xx", ASM(divsd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kPush)
  GEN("r", ASM(push, OP(0)))
  GEN("m", ASM(push, STK(0)))
  GEN("i", ASM(push, OP(0)))
END_RULES

BEGIN_RULES(Instruction::kPop)
  GEN("R", ASM(pop, OP(0)))
  GEN("M", ASM(pop, STK(0)))
END_RULES

BEGIN_RULES(Instruction::kCdq)
  GEN("Rr", ASM(cdq, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kCwd)
  GEN("Rr", ASM(cwd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kCqo)
  GEN("Rr", ASM(cqo, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kExchange)
  GEN("Rr", ASM(xchg, OP(0), OP(1)))
  GEN("Xx", ASM(pxor, OP(0), OP(1)),
            ASM(pxor, OP(1), OP(0)),
            ASM(pxor, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kCmp)
  GEN("rr", ASM(cmp, OP(0), OP(1)))
  GEN("ri", ASM(cmp, OP(0), OP(1)))
  GEN("xx", ASM(comisd, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kTest)
  GEN("rr", ASM(test, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kTest32)
  GEN("rr", ASM(test, REG_OP(0, 32), REG_OP(1, 32)))
END_RULES

BEGIN_RULES(Instruction::kBranch)
  GEN("b", ASM(jmp, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchZ)
  GEN("b", ASM(jz, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNZ)
  GEN("b", ASM(jnz, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchA)
  GEN("b", ASM(ja, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchB)
  GEN("b", ASM(jb, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchAE)
  GEN("b", ASM(jae, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchBE)
  GEN("b", ASM(jbe, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchG)
  GEN("b", ASM(jg, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchL)
  GEN("b", ASM(jl, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchGE)
  GEN("b", ASM(jge, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchLE)
  GEN("b", ASM(jle, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchC)
  GEN("b", ASM(jc, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNC)
  GEN("b", ASM(jnc, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchO)
  GEN("b", ASM(jo, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNO)
  GEN("b", ASM(jno, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchS)
  GEN("b", ASM(js, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNS)
  GEN("b", ASM(jns, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchE)
  GEN("b", ASM(je, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNE)
  GEN("b", ASM(jne, LBL(0)))
END_RULES

#define DEF_COMPARE_OP_RULES(name, fpcomp) \
BEGIN_RULES(Instruction::name) \
  GEN("Rrr", CALL_C(TranslateCompare)) \
  GEN("Rri", CALL_C(TranslateCompare)) \
  GEN("Rrm", CALL_C(TranslateCompare)) \
  if (fpcomp) { \
    GEN("Rxx", CALL_C(TranslateCompare)) \
  } \
END_RULES

DEF_COMPARE_OP_RULES(kEqual, true)
DEF_COMPARE_OP_RULES(kNotEqual, true)
DEF_COMPARE_OP_RULES(kGreaterThanUnsigned, true)
DEF_COMPARE_OP_RULES(kGreaterThanEqualUnsigned, true)
DEF_COMPARE_OP_RULES(kLessThanUnsigned, true)
DEF_COMPARE_OP_RULES(kLessThanEqualUnsigned, true)
DEF_COMPARE_OP_RULES(kGreaterThanSigned, false)
DEF_COMPARE_OP_RULES(kGreaterThanEqualSigned, false)
DEF_COMPARE_OP_RULES(kLessThanSigned, false)
DEF_COMPARE_OP_RULES(kLessThanEqualSigned, false)

#undef DEF_COMPARE_OP_RULES

BEGIN_RULES(Instruction::kInc)
  GEN("r", ASM(inc, OP(0)))
  GEN("m", ASM(inc, STK(0)))
END_RULES

BEGIN_RULES(Instruction::kDec)
  GEN("r", ASM(dec, OP(0)))
  GEN("m", ASM(dec, STK(0)))
END_RULES

BEGIN_RULES(Instruction::kBitTest)
  GEN("ri", ASM(bt, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kYieldInitial)
  GEN(ANY, CALL_C(translateYieldInitial))
END_RULES

BEGIN_RULES(Instruction::kYieldValue)
  GEN(ANY, CALL_C(translateYieldValue))
END_RULES

BEGIN_RULES(Instruction::kStoreGenYieldPoint)
  GEN(ANY, CALL_C(translateStoreGenYieldPoint))
END_RULES

BEGIN_RULES(Instruction::kStoreGenYieldFromPoint)
  GEN(ANY, CALL_C(translateStoreGenYieldFromPoint))
END_RULES

BEGIN_RULES(Instruction::kBranchToYieldExit)
  GEN(ANY, CALL_C(translateBranchToYieldExit))
END_RULES

BEGIN_RULES(Instruction::kResumeGenYield)
  GEN(ANY, CALL_C(translateResumeGenYield))
END_RULES

BEGIN_RULES(Instruction::kYieldExitPoint)
  GEN(ANY, CALL_C(translateYieldExitPoint))
END_RULES

BEGIN_RULES(Instruction::kEpilogueEnd)
  GEN(ANY, CALL_C(translateEpilogueEnd))
END_RULES

BEGIN_RULES(Instruction::kSelect)
  GEN("Rrri", ASM(mov, OP(0), OP(3)),
              ASM(test, OP(1), OP(1)),
              ASM(cmovnz, OP(0), OP(2)))
END_RULES

BEGIN_RULES(Instruction::kIntToBool)
  GEN("Rr", CALL_C(translateIntToBool))
  GEN("Ri", CALL_C(translateIntToBool))
END_RULES

END_RULE_TABLE
// clang-format on
#elif defined(CINDER_AARCH64)

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
  } else if (input->isImm()) {
    as->mov(arch::reg_scratch_br, input->getConstant());
    as->blr(arch::reg_scratch_br);
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
  translateMovExtOp(
      env,
      instr,
      "MovZX",
      [](a64::Builder* as, auto... args) { as->uxtb(args...); },
      [](a64::Builder* as, auto... args) { as->uxth(args...); },
      [](a64::Builder* as, auto... args) { as->ldrb(args...); },
      [](a64::Builder* as, auto... args) { as->ldrh(args...); });
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

// clang-format off
BEGIN_RULE_TABLE

BEGIN_RULES(Instruction::kLea)
  GEN("Rm", CALL_C(translateLea))
END_RULES

BEGIN_RULES(Instruction::kCall)
  GEN("Ri", CALL_C(translateCall))
  GEN("Rr", CALL_C(translateCall))
  GEN("i", CALL_C(translateCall))
  GEN("r", CALL_C(translateCall))
  GEN("m", CALL_C(translateCall))
END_RULES

BEGIN_RULES(Instruction::kMove)
  GEN("Rr", ASM(mov, OP(0), OP(1)))
  GEN("Ri", CALL_C(translateMove))
  GEN("Rm", CALL_C(translateMove))
  GEN("Mr", CALL_C(translateMove))
  GEN("Mi", CALL_C(translateMove))
  GEN("Xx", ASM(fmov, OP(0), OP(1)))
  GEN("Xm", CALL_C(translateMove))
  GEN("Mx", CALL_C(translateMove))
  GEN("Xr", ASM(fmov, OP(0), OP(1)))
  GEN("Rx", ASM(fmov, OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kMoveRelaxed)
  GEN("Rm", CALL_C(translateMove))
  GEN("Mr", CALL_C(translateMove))
  GEN("Mi", CALL_C(translateMove))
END_RULES

BEGIN_RULES(Instruction::kGuard)
  GEN(ANY, CALL_C(TranslateGuard))
END_RULES

BEGIN_RULES(Instruction::kDeoptPatchpoint)
  GEN(ANY, CALL_C(TranslateDeoptPatchpoint))
END_RULES

BEGIN_RULES(Instruction::kNegate)
  GEN("r", CALL_C(translateNegate))
  GEN("Rr", CALL_C(translateNegate))
END_RULES

BEGIN_RULES(Instruction::kInvert)
  GEN("Rr", CALL_C(translateInvert))
END_RULES

BEGIN_RULES(Instruction::kMovZX)
  GEN("Rr", CALL_C(translateMovZX))
  GEN("Rm", CALL_C(translateMovZX))
END_RULES

BEGIN_RULES(Instruction::kMovSX)
  GEN("Rr", CALL_C(translateMovSX))
  GEN("Rm", CALL_C(translateMovSX))
END_RULES

BEGIN_RULES(Instruction::kMovSXD)
  GEN("Rr", CALL_C(translateMovSXD))
  GEN("Rm", CALL_C(translateMovSXD))
END_RULES

BEGIN_RULES(Instruction::kUnreachable)
  GEN(ANY, CALL_C(translateUnreachable))
END_RULES

BEGIN_RULES(Instruction::kAdd)
  GEN("ri", CALL_C(translateAdd))
  GEN("rr", CALL_C(translateAdd))
  GEN("Rri", CALL_C(translateAdd))
  GEN("Rrr", CALL_C(translateAdd))
END_RULES

BEGIN_RULES(Instruction::kSub)
  GEN("ri", CALL_C(translateSub))
  GEN("rr", CALL_C(translateSub))
  GEN("Rri", CALL_C(translateSub))
  GEN("Rrr", CALL_C(translateSub))
END_RULES

BEGIN_RULES(Instruction::kAnd)
  GEN("ri", CALL_C(translateAnd))
  GEN("rr", CALL_C(translateAnd))
  GEN("Rri", CALL_C(translateAnd))
  GEN("Rrr", CALL_C(translateAnd))
END_RULES

BEGIN_RULES(Instruction::kOr)
  GEN("ri", CALL_C(translateOr))
  GEN("rr", CALL_C(translateOr))
  GEN("Rri", CALL_C(translateOr))
  GEN("Rrr", CALL_C(translateOr))
END_RULES

BEGIN_RULES(Instruction::kXor)
  GEN("ri", CALL_C(translateXor))
  GEN("rr", CALL_C(translateXor))
  GEN("Rri", CALL_C(translateXor))
  GEN("Rrr", CALL_C(translateXor))
END_RULES

BEGIN_RULES(Instruction::kMul)
  GEN("rr", CALL_C(translateMul))
  GEN("Rrr", CALL_C(translateMul))
END_RULES

BEGIN_RULES(Instruction::kMulAdd)
  GEN("Rrrr", CALL_C(translateMulAdd))
END_RULES

BEGIN_RULES(Instruction::kDiv)
  GEN("rrr", CALL_C(translateDiv))
  GEN("rr", CALL_C(translateDiv))
  GEN("Rirr", CALL_C(translateDiv))
END_RULES

BEGIN_RULES(Instruction::kDivUn)
  GEN("rrr", CALL_C(translateDivUn))
  GEN("rr", CALL_C(translateDivUn))
  GEN("Rirr", CALL_C(translateDivUn))
END_RULES

BEGIN_RULES(Instruction::kFadd)
  GEN("Xxx", ASM(fadd, OP(0), OP(1), OP(2)))
  GEN("xx", ASM(fadd, OP(0), OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFsub)
  GEN("Xxx", ASM(fsub, OP(0), OP(1), OP(2)))
  GEN("xx", ASM(fsub, OP(0), OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFmul)
  GEN("Xxx", ASM(fmul, OP(0), OP(1), OP(2)))
  GEN("xx", ASM(fmul, OP(0), OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kFdiv)
  GEN("Xxx", ASM(fdiv, OP(0), OP(1), OP(2)))
  GEN("xx", ASM(fdiv, OP(0), OP(0), OP(1)))
END_RULES

BEGIN_RULES(Instruction::kPush)
  GEN("r", CALL_C(translatePush))
  GEN("m", CALL_C(translatePush))
END_RULES

BEGIN_RULES(Instruction::kPop)
  GEN("R", CALL_C(translatePop))
  GEN("M", CALL_C(translatePop))
END_RULES

BEGIN_RULES(Instruction::kExchange)
  GEN("Rr", CALL_C(translateExchange))
  GEN("Xx", CALL_C(translateExchange))
END_RULES

BEGIN_RULES(Instruction::kCmp)
  GEN("rr", CALL_C(translateCmp))
  GEN("ri", CALL_C(translateCmp))
  GEN("xx", CALL_C(translateCmp))
END_RULES

BEGIN_RULES(Instruction::kTest)
  GEN("rr", CALL_C(translateTst))
END_RULES

BEGIN_RULES(Instruction::kTest32)
  GEN("rr", ASM(tst, REG_OP(0, 32), REG_OP(1, 32)))
END_RULES

BEGIN_RULES(Instruction::kBranch)
  GEN("b", ASM(b, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchZ)
  GEN("b", ASM(b_eq, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNZ)
  GEN("b", ASM(b_ne, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchA)
  GEN("b", ASM(b_hi, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchB)
  GEN("b", ASM(b_lo, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchAE)
  GEN("b", ASM(b_hs, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchBE)
  GEN("b", ASM(b_ls, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchG)
  GEN("b", ASM(b_gt, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchL)
  GEN("b", ASM(b_lt, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchGE)
  GEN("b", ASM(b_ge, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchLE)
  GEN("b", ASM(b_le, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchC)
  GEN("b", ASM(b_cs, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNC)
  GEN("b", ASM(b_cc, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchO)
  GEN("b", ASM(b_vs, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNO)
  GEN("b", ASM(b_vc, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchS)
  GEN("b", ASM(b_mi, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNS)
  GEN("b", ASM(b_pl, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchE)
  GEN("b", ASM(b_eq, LBL(0)))
END_RULES

BEGIN_RULES(Instruction::kBranchNE)
  GEN("b", ASM(b_ne, LBL(0)))
END_RULES

#define DEF_COMPARE_OP_RULES(name, fpcomp) \
BEGIN_RULES(Instruction::name) \
  GEN("Rrr", CALL_C(TranslateCompare)) \
  GEN("Rri", CALL_C(TranslateCompare)) \
  if (fpcomp) { \
    GEN("Rxx", CALL_C(TranslateCompare)) \
  } \
END_RULES

DEF_COMPARE_OP_RULES(kEqual, true)
DEF_COMPARE_OP_RULES(kNotEqual, true)
DEF_COMPARE_OP_RULES(kGreaterThanUnsigned, true)
DEF_COMPARE_OP_RULES(kGreaterThanEqualUnsigned, true)
DEF_COMPARE_OP_RULES(kLessThanUnsigned, true)
DEF_COMPARE_OP_RULES(kLessThanEqualUnsigned, true)
DEF_COMPARE_OP_RULES(kGreaterThanSigned, false)
DEF_COMPARE_OP_RULES(kGreaterThanEqualSigned, false)
DEF_COMPARE_OP_RULES(kLessThanSigned, false)
DEF_COMPARE_OP_RULES(kLessThanEqualSigned, false)

#undef DEF_COMPARE_OP_RULES

BEGIN_RULES(Instruction::kInc)
  GEN("r", CALL_C(translateInc))
END_RULES

BEGIN_RULES(Instruction::kDec)
  GEN("r", CALL_C(translateDec))
END_RULES

BEGIN_RULES(Instruction::kBitTest)
  GEN("ri", CALL_C(translateBitTest));
END_RULES

BEGIN_RULES(Instruction::kYieldInitial)
  GEN(ANY, CALL_C(translateYieldInitial))
END_RULES

BEGIN_RULES(Instruction::kYieldValue)
  GEN(ANY, CALL_C(translateYieldValue))
END_RULES

BEGIN_RULES(Instruction::kStoreGenYieldPoint)
  GEN(ANY, CALL_C(translateStoreGenYieldPoint))
END_RULES

BEGIN_RULES(Instruction::kStoreGenYieldFromPoint)
  GEN(ANY, CALL_C(translateStoreGenYieldFromPoint))
END_RULES

BEGIN_RULES(Instruction::kBranchToYieldExit)
  GEN(ANY, CALL_C(translateBranchToYieldExit))
END_RULES

BEGIN_RULES(Instruction::kResumeGenYield)
  GEN(ANY, CALL_C(translateResumeGenYield))
END_RULES

BEGIN_RULES(Instruction::kYieldExitPoint)
  GEN(ANY, CALL_C(translateYieldExitPoint))
END_RULES

BEGIN_RULES(Instruction::kEpilogueEnd)
  GEN(ANY, CALL_C(translateEpilogueEnd))
END_RULES

BEGIN_RULES(Instruction::kSelect)
  GEN("Rrrr", CALL_C(translateSelect))
END_RULES

BEGIN_RULES(Instruction::kIntToBool)
  GEN("Rr", CALL_C(translateIntToBool))
END_RULES

END_RULE_TABLE
// clang-format on
#else

BEGIN_RULE_TABLE
END_RULE_TABLE

#endif

} // namespace jit::codegen::autogen
