// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/generator.h"

extern "C" {
#include "internal/pycore_call.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_intrinsics.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_setobject.h"
#endif

#include "internal/pycore_import.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_pyerrors.h"
#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interpolation.h"
#include "internal/pycore_template.h"
#endif
}

#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/iter_helpers.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/block_builder.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <functional>
#include <sstream>

// XXX: this file needs to be revisited when we optimize HIR-to-LIR translation
// in codegen.cpp/h. Currently, this file is almost an identical copy from
// codegen.cpp with some interfaces changes so that it works with the new
// LIR.

using namespace jit::hir;

namespace jit::lir {

namespace {

#ifndef Py_GIL_DISABLED
constexpr size_t kRefcountOffset = offsetof(PyObject, ob_refcnt);
#endif

// These functions call their counterparts and convert its output from int (32
// bits) to uint64_t (64 bits). This is solely because the code generator cannot
// support an operand size other than 64 bits at this moment. A future diff will
// make it support different operand sizes so that this function can be removed.

extern "C" PyObject* __Invoke_PyList_Extend(
    PyThreadState* tstate,
    PyObject* list,
    PyObject* iterable) {
  if (PyList_Extend(list, iterable) < 0) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        Py_TYPE(iterable)->tp_iter == nullptr && !PySequence_Check(iterable)) {
      _PyErr_Clear(tstate);
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "Value after * must be an iterable, not %.200s",
          Py_TYPE(iterable)->tp_name);
    }
    return nullptr;
  }

  Py_RETURN_NONE;
}

void finishYield(
    BasicBlockBuilder& bbb,
    Instruction* instr,
    const DeoptBase* hir_instr) {
  for (const RegState& rs : hir_instr->live_regs()) {
    instr->addOperands(VReg{bbb.getDefInstr(rs.reg)});
  }
  instr->addOperands(Imm{hir_instr->live_regs().size()});
  instr->addOperands(Imm{bbb.makeDeoptMetadata()});
}

// Checks if a type has reasonable == semantics, that is that
// object identity implies equality when compared by Python.  This
// is true for most types, but not true for floats where nan is
// not equal to nan.  But it is true for container types containing
// those floats where PyObject_RichCompareBool is used and it short
// circuits on object identity.
bool isTypeWithReasonablePointerEq(Type t) {
  return t <= TArray || t <= TBytesExact || t <= TDictExact ||
      t <= TListExact || t <= TSetExact || t <= TTupleExact ||
      t <= TTypeExact || t <= TLongExact || t <= TBool || t <= TFunc ||
      t <= TGen || t <= TNoneType || t <= TSlice;
}

int bytes_from_cint_type(Type type) {
  if (type <= TCInt8 || type <= TCUInt8) {
    return 1;
  } else if (type <= TCInt16 || type <= TCUInt16) {
    return 2;
  } else if (type <= TCInt32 || type <= TCUInt32) {
    return 3;
  } else if (type <= TCInt64 || type <= TCUInt64) {
    return 4;
  }
  JIT_ABORT("Bad primitive int type: ({})", type);
  // NOTREACHED
}

#define FOREACH_FAST_BUILTIN(V) \
  V(Long)                       \
  V(List)                       \
  V(Tuple)                      \
  V(Bytes)                      \
  V(Unicode)                    \
  V(Dict)                       \
  V(Type)

#define INVOKE_CHECK(name)                                       \
  extern "C" uint64_t __Invoke_Py##name##_Check(PyObject* obj) { \
    int result = Py##name##_Check(obj);                          \
    return result == 0 ? 0 : 1;                                  \
  }

FOREACH_FAST_BUILTIN(INVOKE_CHECK)

#undef INVOKE_CHECK

Instruction*
emitSubclassCheck(BasicBlockBuilder& bbb, hir::Register* obj, Type type) {
  // Fast path: a subset of builtin types that have Py_TPFLAGS
  uint64_t fptr = 0;
#define GET_FPTR(name)                                            \
  if (type <= T##name) {                                          \
    fptr = reinterpret_cast<uint64_t>(__Invoke_Py##name##_Check); \
  } else
  FOREACH_FAST_BUILTIN(GET_FPTR) {
    JIT_ABORT("Unsupported subclass check in CondBranchCheckType");
  }
#undef GET_FPTR
  return bbb.appendInstr(
      Instruction::kCall,
      OutVReg{OperandBase::k8bit},
      // TASK(T140174965): This should be MemImm.
      Imm{fptr},
      obj);
}

#undef FOREACH_FAST_BUILTIN

Py_ssize_t frameOffsetBefore(const BeginInlinedFunction* instr) {
  Py_ssize_t depth = 0;
  for (auto frame = instr->callerFrameState(); frame != nullptr;
       frame = frame->parent) {
    depth -= frameHeaderSize(frame->code);
  }
  return depth;
}

Py_ssize_t frameOffsetOf(const BeginInlinedFunction* instr) {
  return frameOffsetBefore(instr) - frameHeaderSize(instr->code());
}

// Update the global ref count total after an Inc or Dec operation.
void updateRefTotal(BasicBlockBuilder& bbb, Instruction::Opcode op) {
  if (kPyRefDebug) {
    auto helper = reinterpret_cast<uint64_t>(
        op == Instruction::Opcode::kInc ? JITRT_IncRefTotal
                                        : JITRT_DecRefTotal);
    bbb.appendInstr(Instruction::kCall, Imm{helper});
  }
}

} // namespace

LIRGenerator::LIRGenerator(
    const jit::hir::Function* func,
    jit::codegen::Environ* env)
    : func_(func),
      env_(env),
      is_gen_(
          func->code != nullptr &&
          (func->code->co_flags & kCoFlagsAnyGenerator)) {
  for (int i = 0, n = func->env.numLoadTypeAttrCaches(); i < n; i++) {
    load_type_attr_caches_.emplace_back(
        getContext()->allocateLoadTypeAttrCache());
  }
  for (int i = 0, n = func->env.numLoadTypeMethodCaches(); i < n; i++) {
    load_type_method_caches_.emplace_back(
        getContext()->allocateLoadTypeMethodCache());
  }
}

BasicBlock* LIRGenerator::GenerateEntryBlock() {
  // Entry block: placeholder for argument loading (currently handled by
  // raw asmjit in generatePrologue).
  entry_block_ = lir_func_->allocateBasicBlock();

  // Frame setup block: allocate stack space, save callee-saved registers,
  // and bind initial register values.
  frame_setup_block_ = lir_func_->allocateBasicBlock();

  // SetupFrame: allocate stack space and save callee-saved registers.
  // Translated post-regalloc using frame info from Environ.
  frame_setup_block_->allocateInstr(Instruction::kSetupFrame, nullptr);

  auto bindVReg = [&](PhyLocation phy_reg) {
    auto instr = frame_setup_block_->allocateInstr(Instruction::kBind, nullptr);
    instr->output()->setVirtualRegister();
    instr->allocatePhyRegisterInput(phy_reg);
    return instr;
  };

  env_->asm_extra_args = bindVReg(codegen::INITIAL_EXTRA_ARGS_REG);
  env_->asm_tstate = bindVReg(codegen::INITIAL_TSTATE_REG);
  env_->asm_func = bindVReg(codegen::INITIAL_FUNC_REG);

  entry_block_->addSuccessor(frame_setup_block_);

  return entry_block_;
}

namespace {

// Set a pending annotation that labels a section of generated code
// in PYTHONJITDUMPASM=1 output. The annotation covers all subsequent
// instructions until the next annotation or end of block.
void emitAnnotation(BasicBlock* bb, std::string text) {
  bb->pending_annotation_ = std::move(text);
}

} // namespace

void PopulateResumeEntryBlock(BasicBlock* bb, Py_ssize_t gi_jit_data_offset) {
  using DT = DataType;

  auto gen_reg = codegen::ARGUMENT_REGS[0];
#if defined(CINDER_X86_64) && defined(_WIN32)
  // On Windows, R8 and R9 are ARGUMENT_REGS[2] and [3] (finish_yield_from
  // and tstate). Use R10/R11 to avoid clobbering them before the resume
  // target reads them.
  PhyLocation scratch{10, 64}; // r10
  PhyLocation jit_data_reg{11, 64}; // r11
#else
  PhyLocation scratch{8, 64}; // r8/x8
  PhyLocation jit_data_reg{9, 64}; // r9/x9
#endif
  auto fp_reg = codegen::arch::reg_frame_pointer_loc;

  auto gi_off = static_cast<int32_t>(gi_jit_data_offset);
  auto link_off = static_cast<int32_t>(offsetof(GenDataFooter, linkAddress));
  auto ret_off = static_cast<int32_t>(offsetof(GenDataFooter, returnAddress));
  auto orig_fp_off =
      static_cast<int32_t>(offsetof(GenDataFooter, originalFramePointer));
  auto yp_off = static_cast<int32_t>(offsetof(GenDataFooter, yieldPoint));
  auto rt_off = static_cast<int32_t>(GenYieldPoint::resumeTargetOffset());

  // Prologue: push fp, set up frame.
  bb->allocateInstr(Instruction::kPrologue, nullptr);

  // SetupFrame: allocate stack, save callee-saved regs
  bb->allocateInstr(Instruction::kSetupFrame, nullptr);

  // jit_data = gen->gi_jit_data
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg(jit_data_reg),
      Ind(gen_reg, gi_off));

  // scratch = [fp + 0]  (saved frame pointer / link address)
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(scratch), Ind(fp_reg));

  // footer->linkAddress = scratch
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(jit_data_reg, link_off),
      PhyReg(scratch));

  // scratch = [fp + 8]  (return address)
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(scratch), Ind(fp_reg, (int32_t)8));

  // footer->returnAddress = scratch
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(jit_data_reg, ret_off),
      PhyReg(scratch));

  // footer->originalFramePointer = fp
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(jit_data_reg, orig_fp_off),
      PhyReg(fp_reg));

  // fp = jit_data  (switch to generator's frame storage)
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(fp_reg), PhyReg(jit_data_reg));

  // scratch = footer->yieldPoint
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(scratch), Ind(fp_reg, yp_off));

  // footer->yieldPoint = NULL
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutInd(fp_reg, yp_off, DT::kObject), Imm(0));

  // Jump to yieldPoint->resumeTarget
  bb->allocateInstr(Instruction::kIndirectJump, nullptr, Ind(scratch, rt_off));
}

void PopulateEntryBlock(
    BasicBlock* entry_block,
    const std::vector<PhyLocation>& arg_locations) {
  auto args_reg = codegen::INITIAL_EXTRA_ARGS_REG;

  emitAnnotation(entry_block, "Load vectorcall arguments");

  // Move the args pointer from the calling convention register (rsi) to the
  // internal register (r10) used by the rest of the function.
  entry_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{args_reg},
      PhyReg{codegen::ARGUMENT_REGS[1]});

  auto kPointerSize = static_cast<int32_t>(sizeof(void*));

  bool has_extra_args = false;
  for (size_t i = 0; i < arg_locations.size(); i++) {
    PhyLocation arg = arg_locations[i];
    if (arg == PhyLocation::REG_INVALID) {
      has_extra_args = true;
      continue;
    }
    if (arg.is_gp_register()) {
      entry_block->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg(arg),
          Ind(args_reg, static_cast<int32_t>(i * kPointerSize)));
    } else {
      entry_block->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg(arg, OperandBase::kDouble),
          Ind(args_reg,
              static_cast<int32_t>(i * kPointerSize),
              OperandBase::kDouble));
    }
  }
  if (has_extra_args) {
    // Point extra_args register past the register-bound args to the
    // start of the overflow args in the vectorcall array.
    auto offset = static_cast<int32_t>(
        (codegen::ARGUMENT_REGS.size() - 1) * kPointerSize);
    entry_block->allocateInstr(
        Instruction::kLea, nullptr, OutPhyReg(args_reg), Ind(args_reg, offset));
  }
}

UnresolvedJumpTable GenerateStaticTypeCheckBlocks(
    Function* lir_func,
    BasicBlock* entry_block,
    const std::vector<hir::TypedArgument>& typed_args,
    int num_args,
    CodeRuntime* code_rt,
    asmjit::Label failure_label) {
  if (typed_args.empty()) {
    return {};
  }

  // Register assignments:
  //   ARGUMENT_REGS[1] — args array pointer (read-only)
  //   ARGUMENT_REGS[3] — defaulted arg count (read-only)
  //   TYPECHECK_SCRATCH_REG — main scratch register (an arg that's not passed)
  //   INITIAL_EXTRA_ARGS_REG — type pointer for MRO search
  //   INITIAL_TSTATE_REG — MRO end pointer
  //   arch::reg_scratch_0_loc — compare scratch
  auto args_reg = codegen::ARGUMENT_REGS[1];
  auto defaulted_count_reg = codegen::ARGUMENT_REGS[3];
  auto tc_scratch = codegen::ARGUMENT_REGS[4];
  auto mro_type = codegen::INITIAL_EXTRA_ARGS_REG;
  auto mro_end = codegen::INITIAL_TSTATE_REG;
  auto scratch = codegen::arch::reg_scratch_0_loc;

  // Keep type objects alive.
  for (const auto& arg : typed_args) {
    code_rt->addReference(BorrowedRef(arg.pytype));
  }

  // Allocate the dispatch block first so it is laid out before all type check
  // blocks. The reentry short jump targets this block and must reach it within
  // ±127 bytes.
  auto* dispatch_block = lir_func->allocateBasicBlock();

  // Allocate check blocks and their MRO blocks interleaved so each MRO loop
  // block immediately follows its corresponding check block in the layout.
  // check_blocks[0] corresponds to typed_args[checks.size()-1] (the last arg).
  std::vector<BasicBlock*> check_blocks;
  std::vector<BasicBlock*> mro_blocks;
  check_blocks.reserve(typed_args.size());
  mro_blocks.reserve(typed_args.size());
  for (size_t k = 0; k < typed_args.size(); k++) {
    check_blocks.push_back(lir_func->allocateBasicBlock());
    Py_ssize_t i = typed_args.size() - 1 - k;
    const auto& arg = typed_args[i];
    if (!arg.exact && (arg.threadSafeTpFlags() & Py_TPFLAGS_BASETYPE)) {
      mro_blocks.push_back(lir_func->allocateBasicBlock());
    } else {
      mro_blocks.push_back(nullptr);
    }
  }

  // Build the dispatch mapping: for each defaulted_arg_count value, determine
  // which check block to jump to (same logic as the original jump table).
  std::vector<std::pair<int, BasicBlock*>> dispatch_entries;
  JIT_DCHECK(check_blocks.size() > 0, "allocated above");
  BasicBlock* next_target = check_blocks[0];
  Py_ssize_t ci = typed_args.size() - 1;
  int dac = 0;
  // dispatch_targets[dac] = block to jump to for that defaulted count
  // We iterate like the original: for each dac, emit a jump to next_target,
  // then potentially advance next_target when we hit a checked arg boundary.
  while (dac < num_args) {
    dispatch_entries.emplace_back(dac, next_target);

    if (ci >= 0) {
      long local = typed_args[ci].locals_idx;
      if (num_args - dac - 1 == local) {
        if (ci == 0) {
          next_target = entry_block;
        } else {
          ci--;
          // check_blocks index: typed_args are checked last-to-first,
          // so check_blocks[k] corresponds to typed_args[size-1-k]
          JIT_DCHECK(
              check_blocks.size() > typed_args.size() - 1 - ci,
              "allocated above");
          next_target = check_blocks[typed_args.size() - 1 - ci];
        }
      }
    }
    dac++;
  }

  // Allocate a jump table in CodeRuntime (num_args + 1 entries for
  // defaulted_arg_count = 0 through num_args).
  auto* jump_table = code_rt->allocateTypeCheckJumpTable(num_args + 1);
  auto table_addr = reinterpret_cast<uint64_t>(jump_table);

  // Build the unresolved jump table for post-codegen resolution.
  UnresolvedJumpTable result;
  result.table = jump_table;
  result.dispatch_block = dispatch_block;
  for (const auto& [count, target] : dispatch_entries) {
    result.entries.emplace_back(count, target);
  }
  // Entry for all-defaulted case (dac == num_args → skip all checks).
  result.entries.emplace_back(num_args, entry_block);

  // Emit the dispatch block: index into jump table and indirect jump.
  emitAnnotation(dispatch_block, "Static type check dispatch");
  dispatch_block->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(defaulted_count_reg), Imm{0});
  dispatch_block->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg(tc_scratch), Imm(table_addr));
  dispatch_block->allocateInstr(
      Instruction::kLea,
      nullptr,
      OutPhyReg(tc_scratch),
      Ind(tc_scratch, defaulted_count_reg, 8, 0));
  dispatch_block->allocateInstr(
      Instruction::kIndirectJump, nullptr, Ind(tc_scratch));

  // --- Emit check blocks ---
  // check_blocks[k] checks typed_args[typed_args.size()-1-k]
  // (from last arg to first, matching original order).
  for (size_t k = 0; k < check_blocks.size(); k++) {
    Py_ssize_t i = typed_args.size() - 1 - k;
    auto* bb = check_blocks[k];
    const auto& arg = typed_args[i];

    // Determine the next_check target: success goes to the next check block,
    // or entry_block if this is the last check (i == 0).
    BasicBlock* next_check =
        (k + 1 < check_blocks.size()) ? check_blocks[k + 1] : entry_block;

    // Annotate this check block with the arg index and type name.
    {
      std::string label = fmt::format(
          "Type check: arg[{}] ({})", arg.locals_idx, arg.pytype->tp_name);
      if (!arg.exact && (arg.threadSafeTpFlags() & Py_TPFLAGS_BASETYPE)) {
        label += " (base type, MRO walk)";
      }
      emitAnnotation(bb, std::move(label));
    }

    // Load arg value from vectorcall array: tc_scratch = args[locals_idx]
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg(tc_scratch),
        Ind(args_reg, static_cast<int32_t>(arg.locals_idx * 8)));

    // Load type: tc_scratch = tc_scratch->ob_type
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg(tc_scratch),
        Ind(tc_scratch, static_cast<int32_t>(offsetof(PyObject, ob_type))));

    // If optional: check if None
    if (arg.optional) {
      auto none_type_val = reinterpret_cast<uint64_t>(Py_TYPE(Py_None));
      bb->allocateInstr(
          Instruction::kMove, nullptr, OutPhyReg(scratch), Imm(none_type_val));
      bb->allocateInstr(
          Instruction::kCmp, nullptr, PhyReg(tc_scratch), PhyReg(scratch));
      bb->allocateInstr(Instruction::kBranchE, nullptr, Lbl(next_check));
      bb->addSuccessor(next_check);
    }

    // Check exact type match: cmp tc_scratch, arg.pytype
    auto type_val = reinterpret_cast<uint64_t>(arg.pytype.get());
    bb->allocateInstr(
        Instruction::kMove, nullptr, OutPhyReg(scratch), Imm(type_val));
    bb->allocateInstr(
        Instruction::kCmp, nullptr, PhyReg(tc_scratch), PhyReg(scratch));
    bb->allocateInstr(Instruction::kBranchE, nullptr, Lbl(next_check));
    if (!arg.optional) {
      // Only add successor once
      bb->addSuccessor(next_check);
    }

    if (!arg.exact && (arg.threadSafeTpFlags() & Py_TPFLAGS_BASETYPE)) {
      // Need MRO walk. Branch to the MRO loop block.
      JIT_DCHECK(mro_blocks.size() > k, "allocated above");
      auto* mro_bb = mro_blocks[k];
      JIT_CHECK(mro_bb != nullptr, "Expected MRO block for check {}", k);

      // Setup: mro_type = type pointer
      bb->allocateInstr(
          Instruction::kMove, nullptr, OutPhyReg(mro_type), Imm(type_val));

      // tc_scratch = tc_scratch->tp_mro (load MRO tuple)
      bb->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg(tc_scratch),
          Ind(tc_scratch,
              static_cast<int32_t>(offsetof(PyTypeObject, tp_mro))));

      // mro_end = tc_scratch->ob_size (tuple length)
      bb->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg(mro_end),
          Ind(tc_scratch,
              static_cast<int32_t>(offsetof(PyVarObject, ob_size))));

      // tc_scratch = &tc_scratch->ob_item[0]
      bb->allocateInstr(
          Instruction::kAdd,
          nullptr,
          PhyReg(tc_scratch),
          Imm(static_cast<uint64_t>(offsetof(PyTupleObject, ob_item))));

      // mro_end = tc_scratch + mro_end * 8 (end pointer)
      bb->allocateInstr(
          Instruction::kLea,
          nullptr,
          OutPhyReg(mro_end),
          Ind(tc_scratch, mro_end, 8, 0));

      // Branch to MRO loop
      bb->allocateInstr(Instruction::kBranch, nullptr, Lbl(mro_bb));
      bb->addSuccessor(mro_bb);

      // --- MRO loop block ---
      // Loop: scratch = *tc_scratch; cmp scratch, mro_type; je next_check;
      //        tc_scratch += 8; cmp tc_scratch, mro_end; jne loop; jmp failure
      emitAnnotation(mro_bb, fmt::format("MRO walk: arg[{}]", arg.locals_idx));
      mro_bb->allocateInstr(
          Instruction::kMove, nullptr, OutPhyReg(scratch), Ind(tc_scratch));
      mro_bb->allocateInstr(
          Instruction::kCmp, nullptr, PhyReg(scratch), PhyReg(mro_type));
      mro_bb->allocateInstr(Instruction::kBranchE, nullptr, Lbl(next_check));
      mro_bb->addSuccessor(next_check);

      mro_bb->allocateInstr(
          Instruction::kAdd,
          nullptr,
          PhyReg(tc_scratch),
          Imm(static_cast<uint64_t>(sizeof(PyObject*))));
      mro_bb->allocateInstr(
          Instruction::kCmp, nullptr, PhyReg(tc_scratch), PhyReg(mro_end));
      mro_bb->allocateInstr(Instruction::kBranchNE, nullptr, Lbl(mro_bb));
      mro_bb->addSuccessor(mro_bb); // self-loop

      // Fall through to failure
      mro_bb->allocateInstr(
          Instruction::kBranch, nullptr, AsmLbl(failure_label));

    } else {
      // No MRO walk needed — type mismatch is a failure.
      bb->allocateInstr(Instruction::kBranch, nullptr, AsmLbl(failure_label));
    }
  }

  return result;
}

void GenerateArgcountCheckBlocks(
    Function* lir_func,
    const hir::Function* func,
    asmjit::Label correct_args_label,
    asmjit::Label prologue_exit) {
  bool returns_primitive_double = func->returnsPrimitiveDouble();
  BorrowedRef<PyCodeObject> code = func->code;
  bool have_varargs = code->co_flags & (CO_VARARGS | CO_VARKEYWORDS);
  bool will_check_argcount = !have_varargs && code->co_kwonlyargcount == 0;
  int num_args = func->numArgs();

  // Register assignments (vectorcall convention):
  //   ARGUMENT_REGS[0] — callable (PyObject*)
  //   ARGUMENT_REGS[1] — args array (PyObject**)
  //   ARGUMENT_REGS[2] — nargsf (Py_ssize_t, includes
  //   PY_VECTORCALL_ARGUMENTS_OFFSET) ARGUMENT_REGS[3] — kwnames (PyObject*
  //   tuple or nullptr)
  auto kwnames_reg = codegen::ARGUMENT_REGS[3];
  auto nargsf_reg = codegen::ARGUMENT_REGS[2];

  if (will_check_argcount) {
    // Two blocks:
    //   kw_dispatch: test kwnames → if null, branch to argcount_check;
    //                otherwise call JITRT_CallWithKeywordArgs → exit.
    //   argcount_check: cmp nargsf with numArgs → if equal, branch to
    //                   next_block; otherwise call incorrect-argcount
    //                   helper → exit.
    auto* kw_dispatch = lir_func->allocateBasicBlock();
    auto* argcount_check = lir_func->allocateBasicBlock();

    // --- kw_dispatch ---
    emitAnnotation(kw_dispatch, "Keyword argument dispatch");

    kw_dispatch->allocateInstr(
        Instruction::kTest, nullptr, PhyReg{kwnames_reg}, PhyReg{kwnames_reg});
    kw_dispatch->allocateInstr(
        Instruction::kBranchZ, nullptr, Lbl{argcount_check});
    kw_dispatch->addSuccessor(argcount_check);

    kw_dispatch->allocateInstr(
        Instruction::kCall,
        nullptr,
        // Args are stack allocated in the simple version so only
        // use if we have a reasonable number of arguments.
        Imm{reinterpret_cast<uint64_t>(
            num_args < 30 ? JITRT_CallWithKeywordArgsSimple
                          : JITRT_CallWithKeywordArgs)});
    kw_dispatch->allocateInstr(
        Instruction::kBranch, nullptr, AsmLbl{prologue_exit});

    // --- argcount_check ---
    emitAnnotation(argcount_check, "Check if called with correct argcount");

    // Load numArgs into kwnames_reg. After the kBranchZ above we know
    // kwnames is null so this register is free. This also sets up the
    // 4th argument for JITRT_CallWithIncorrectArgcount if needed.
    argcount_check->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{kwnames_reg},
        Imm{static_cast<uint64_t>(num_args)});

    // 32-bit compare: nargsf (low 32 bits = actual argcount, upper bits
    // may contain PY_VECTORCALL_ARGUMENTS_OFFSET) vs numArgs.
    argcount_check->allocateInstr(
        Instruction::kCmp,
        nullptr,
        PhyReg{nargsf_reg, OperandBase::k32bit},
        PhyReg{kwnames_reg, OperandBase::k32bit});
    argcount_check->allocateInstr(
        Instruction::kBranchE, nullptr, AsmLbl{correct_args_label});

    // Wrong argcount: kwnames_reg already holds numArgs for the 4th arg.
    auto helper = returns_primitive_double
        ? reinterpret_cast<uint64_t>(JITRT_CallWithIncorrectArgcountFPReturn)
        : reinterpret_cast<uint64_t>(JITRT_CallWithIncorrectArgcount);
    argcount_check->allocateInstr(Instruction::kCall, nullptr, Imm{helper});
    argcount_check->allocateInstr(
        Instruction::kBranch, nullptr, AsmLbl{prologue_exit});

  } else {
    // have_varargs or kwonlyargcount > 0: always dispatch through the
    // keyword helper (it handles *args, **kwargs, keyword-only args, etc.).
    auto* kw_dispatch = lir_func->allocateBasicBlock();

    emitAnnotation(kw_dispatch, "Keyword argument dispatch (varargs/kwonly)");

    kw_dispatch->allocateInstr(
        Instruction::kCall,
        nullptr,
        Imm{reinterpret_cast<uint64_t>(JITRT_CallWithKeywordArgs)});
    kw_dispatch->allocateInstr(
        Instruction::kBranch, nullptr, AsmLbl{prologue_exit});
  }
}

void GenerateFunctionEntryBlock(Function* lir_func) {
  auto* block = lir_func->allocateBasicBlock();

  // No annotation needed — translatePrologue adds "Set up frame pointer".
  block->allocateInstr(Instruction::kPrologue, nullptr);
}

void GeneratePrimitiveArgsPrologueBlock(
    Function* lir_func,
    PyObject* prim_args_info,
    bool returns_primitive_double,
    asmjit::Label prologue_exit) {
  // The primitive-args prologue loads the _PyTypedArgsInfo* into the 5th
  // argument register (ARGUMENT_REGS[4] = R8 on x86-64, x4 on aarch64),
  // calls the appropriate JITRT_CallStaticallyWithPrimitiveSignature helper,
  // then exits. If the helper decides the call can proceed statically, it
  // returns to the reentry point; otherwise it handles the call itself and
  // the prologue_exit stub tears down the frame and returns.
  auto arg4_reg = codegen::ARGUMENT_REGS[4];

  auto* block = lir_func->allocateBasicBlock();

  emitAnnotation(block, "Primitive args prologue");

  // Load _PyTypedArgsInfo* into ARGUMENT_REGS[4].
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arg4_reg},
      Imm{reinterpret_cast<uint64_t>(prim_args_info)});

  // Call the appropriate helper.
  auto helper = returns_primitive_double
      ? reinterpret_cast<uint64_t>(JITRT_CallStaticallyWithPrimitiveSignatureFP)
      : reinterpret_cast<uint64_t>(JITRT_CallStaticallyWithPrimitiveSignature);

  block->allocateInstr(Instruction::kCall, nullptr, Imm{helper});

  // The helper either handled the call (result in return register) and we
  // exit, or it set up args for the normal path and we fall through.
  // Since the original code unconditionally calls generateFunctionExit()
  // after the call, we branch to prologue_exit.
  block->allocateInstr(Instruction::kBranch, nullptr, AsmLbl{prologue_exit});
}

void GenerateBoxedReturnWrapperBlocks(
    Function* lir_func,
    hir::Type return_type,
    asmjit::Label& generic_entry,
    asmjit::Label& wrapper_exit) {
  using codegen::ARGUMENT_REGS;
  namespace arch = codegen::arch;

  bool returns_double = return_type <= TCDouble;
  auto aux_return_reg = arch::reg_general_auxilary_return_loc;
  auto ret64 = arch::reg_general_return_loc;

#if defined(CINDER_X86_64)
  PhyLocation ret8 = codegen::AL;
  PhyLocation ret16 = codegen::AX;
#elif defined(CINDER_AARCH64)
  PhyLocation ret8 = codegen::W0;
  PhyLocation ret16 = codegen::W0;
#endif

  auto* wrapper_entry = lir_func->allocateBasicBlock();
  auto* box_block = lir_func->allocateBasicBlock();

  // --- wrapper_entry ---
  emitAnnotation(wrapper_entry, "Boxed return wrapper");

  // Set up a minimal frame for the wrapper itself.
  wrapper_entry->allocateInstr(Instruction::kPrologue, nullptr);

  // Call the inner JIT function.
  wrapper_entry->allocateInstr(
      Instruction::kCall, nullptr, AsmLbl{generic_entry});

  // Check the success flag. For integer returns this is in EDX/W1; for
  // double returns it is in XMM1/D1 (move it to a GP register first).
  if (returns_double) {
    wrapper_entry->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{aux_return_reg},
        PhyReg{arch::reg_double_auxilary_return_loc, OperandBase::kDouble});
  }

  auto test_size = returns_double ? OperandBase::k64bit : OperandBase::k32bit;
  wrapper_entry->allocateInstr(
      Instruction::kTest,
      nullptr,
      PhyReg{aux_return_reg, test_size},
      PhyReg{aux_return_reg, test_size});
  wrapper_entry->allocateInstr(Instruction::kBranchNZ, nullptr, Lbl{box_block});
  wrapper_entry->addSuccessor(box_block);

  // Error path: for doubles, zero out RAX/X0 (integer error path already
  // has the correct NULL in RAX from the inner function).
  if (returns_double) {
    wrapper_entry->allocateInstr(
        Instruction::kMove, nullptr, OutPhyReg{ret64}, Imm{0});
  }
  wrapper_entry->allocateInstr(
      Instruction::kBranch, nullptr, AsmLbl{wrapper_exit});

  // --- box_block ---
  emitAnnotation(box_block, "Box primitive return value");

  // Determine the box function and any sign/zero extension needed to
  // move the return value into the first argument register.
  uint64_t box_func;

  if (returns_double) {
    // XMM0/D0 already holds the double; the box function reads it from there.
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxDouble);
  } else {
    // Move the primitive result from the return register (RAX/X0) to the
    // first argument register (RDI on x86-64, X0 on aarch64).
    // On aarch64 return reg == arg reg, so 32/64-bit types need no move.
    if (return_type <= TCBool) {
      box_block->allocateInstr(
          Instruction::kMovZX,
          nullptr,
          OutPhyReg{ARGUMENT_REGS[0], OperandBase::k32bit},
          PhyReg{ret8, OperandBase::k8bit});
      box_func = reinterpret_cast<uint64_t>(JITRT_BoxBool);
    } else if (return_type <= TCInt8) {
      box_block->allocateInstr(
          Instruction::kMovSX,
          nullptr,
          OutPhyReg{ARGUMENT_REGS[0], OperandBase::k32bit},
          PhyReg{ret8, OperandBase::k8bit});
      box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
    } else if (return_type <= TCUInt8) {
      box_block->allocateInstr(
          Instruction::kMovZX,
          nullptr,
          OutPhyReg{ARGUMENT_REGS[0], OperandBase::k32bit},
          PhyReg{ret8, OperandBase::k8bit});
      box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
    } else if (return_type <= TCInt16) {
      box_block->allocateInstr(
          Instruction::kMovSX,
          nullptr,
          OutPhyReg{ARGUMENT_REGS[0], OperandBase::k32bit},
          PhyReg{ret16, OperandBase::k16bit});
      box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
    } else if (return_type <= TCUInt16) {
      box_block->allocateInstr(
          Instruction::kMovZX,
          nullptr,
          OutPhyReg{ARGUMENT_REGS[0], OperandBase::k32bit},
          PhyReg{ret16, OperandBase::k16bit});
      box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
    } else if (
        return_type <= TCInt32 || return_type <= TCUInt32 ||
        return_type <= TCInt64 || return_type <= TCUInt64) {
      // All 32/64-bit integer types: full-width move from return register
      // to first argument register. On aarch64 these are the same register
      // so no move is needed.
      if (ARGUMENT_REGS[0] != ret64) {
        box_block->allocateInstr(
            Instruction::kMove,
            nullptr,
            OutPhyReg{ARGUMENT_REGS[0]},
            PhyReg{ret64});
      }
      if (return_type <= TCInt32) {
        box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
      } else if (return_type <= TCUInt32) {
        box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
      } else if (return_type <= TCInt64) {
        box_func = reinterpret_cast<uint64_t>(JITRT_BoxI64);
      } else {
        box_func = reinterpret_cast<uint64_t>(JITRT_BoxU64);
      }
    } else {
      JIT_ABORT("Unsupported primitive return type {}", return_type.toString());
    }
  }

  box_block->allocateInstr(Instruction::kCall, nullptr, Imm{box_func});
  box_block->allocateInstr(Instruction::kBranch, nullptr, AsmLbl{wrapper_exit});
}

void LIRGenerator::GenerateExitBlocks() {
  if (!is_gen_) {
    auto* block = lir_func_->allocateBasicBlock();
    exit_block_ = block;

    // func_->code may be null in unit tests that parse HIR directly.
    if (func_->code == nullptr) {
      exit_phi_ = block->allocateInstr(
          Instruction::kPhi, nullptr, OutVReg{DataType::kObject});
      block->allocateInstr(Instruction::kEpilogueEnd, nullptr, VReg{exit_phi_});
      return;
    }

    auto ret_data_type = hirTypeToDataType(func_->return_type);
    exit_phi_ = block->allocateInstr(
        Instruction::kPhi, nullptr, OutVReg{ret_data_type});

    // Unlink frame before epilogue. Non-generators always unlink.
    bool has_freevars = func_->code != nullptr && func_->code->co_nfreevars > 0;
    bool uses_lw_frames = getConfig().frame_mode == FrameMode::kLightweight;
    uint64_t helper;
    if (!env_->can_deopt && uses_lw_frames && !has_freevars) {
      helper = reinterpret_cast<uint64_t>(JITRT_UnlinkLeafFrame);
    } else if (!has_freevars && uses_lw_frames) {
      helper = reinterpret_cast<uint64_t>(JITRT_UnlinkLightweightFrameFast);
    } else {
      helper = reinterpret_cast<uint64_t>(JITRT_UnlinkFrame);
    }
    block->allocateInstr(
        Instruction::kCall, nullptr, Imm{helper}, VReg{env_->asm_tstate});

    block->allocateInstr(Instruction::kEpilogueEnd, nullptr, VReg{exit_phi_});
    return;
  }

  // For generators, create exit blocks:
  //   exit_block_: phi for return value + gen completion state + branch
  //   exit_epilogue_: phi merging return and yield values + unlink + epilogue
  auto ret_data_type = hirTypeToDataType(func_->return_type);

  auto* block = lir_func_->allocateBasicBlock();
  exit_block_ = block;

  // Phi for the return value entering this block.
  exit_phi_ =
      block->allocateInstr(Instruction::kPhi, nullptr, OutVReg{ret_data_type});

  // Returning from a generator, it is now complete.
  // 3.12+: load gen pointer from GenDataFooter, then store gi_frame_state
  auto* gen_ptr = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg{},
      Ind{codegen::arch::reg_frame_pointer_loc,
          static_cast<int32_t>(offsetof(GenDataFooter, gen))});

  auto* gi_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{
          gen_ptr, static_cast<int32_t>(offsetof(PyGenObject, gi_frame_state))},
      Imm{static_cast<uint64_t>(
#if PY_VERSION_HEX >= 0x030F0000
              FRAME_CLEARED
#else
              FRAME_COMPLETED
#endif
              ),
          DataType::k8bit});
  static_assert(sizeof(PyGenObject::gi_frame_state) == 1);
  gi_store->output()->setDataType(DataType::k8bit);

  // Branch from exit_block_ to exit_epilogue_.
  block->allocateInstr(Instruction::kReturn, nullptr);

  // Create the shared epilogue block for generators.
  exit_epilogue_ = lir_func_->allocateBasicBlock();
  block->addSuccessor(exit_epilogue_);

  // Phi merging return values (from exit_block_) and yield values.
  epilogue_phi_ = exit_epilogue_->allocateInstr(
      Instruction::kPhi, nullptr, OutVReg{ret_data_type});

  block = exit_epilogue_;

  // Restore original frame pointer. Must come after JITRT_UnlinkFrame
  // (if present) since changing RBP makes regalloc spill slots
  // inaccessible.
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{codegen::arch::reg_frame_pointer_loc},
      Ind{codegen::arch::reg_frame_pointer_loc,
          static_cast<int32_t>(offsetof(GenDataFooter, originalFramePointer))});

  block->allocateInstr(Instruction::kEpilogueEnd, nullptr, VReg{epilogue_phi_});
}

void LIRGenerator::AnalyzeCopies() {
  // Find all HIR instructions in the input that would end with a copy, and
  // assign their output the same vreg as the input, effectively performing
  // copy propagation during lowering.
  //
  // We should really be emitting copies during lowering and eliminating them
  // after the fact, to keep this information localized to the lowering code.

  for (auto& block : func_->cfg.blocks) {
    for (auto& instr : block) {
      // Cast doesn't have to be a special case once it deopts and always
      // returns its input.
      if (instr.output() != nullptr && !instr.IsCast() &&
          hir::isPassthrough(instr)) {
        env_->copy_propagation_map.emplace(instr.output(), instr.GetOperand(0));
      }
    }
  }
}

std::unique_ptr<jit::lir::Function> LIRGenerator::TranslateFunction() {
  AnalyzeCopies();

  auto function = std::make_unique<jit::lir::Function>(func_);
  lir_func_ = function.get();

  // generate entry block and exit block
  entry_block_ = GenerateEntryBlock();

#if defined(CINDER_AARCH64)
  // Compute the caller FrameState and active code object at each block's entry
  // by walking the CFG. Needed because blocks inside an inlined function may
  // not contain BeginInlinedFunction themselves (they're reached via branches).
  struct BlockInlineCtx {
    const hir::FrameState* caller_fs{nullptr};
    BorrowedRef<PyCodeObject> code{nullptr};
  };
  UnorderedMap<const hir::BasicBlock*, BlockInlineCtx> block_inline_ctx;
  if (func_->code != nullptr &&
      getConfig().frame_mode == FrameMode::kLightweight) {
    auto hir_entry_block = GetHIRFunction()->cfg.entry_block;
    std::vector<const hir::BasicBlock*> bfs;
    bfs.push_back(hir_entry_block);
    block_inline_ctx[hir_entry_block] = {nullptr, func_->code};
    for (size_t bi = 0; bi < bfs.size(); ++bi) {
      const auto* block = bfs[bi];
      auto ctx = block_inline_ctx[block];
      for (const auto& instr : *block) {
        if (instr.opcode() == Opcode::kBeginInlinedFunction) {
          auto bif = static_cast<const BeginInlinedFunction*>(&instr);
          ctx.caller_fs = bif->callerFrameState();
          ctx.code = bif->code();
        } else if (instr.opcode() == Opcode::kEndInlinedFunction) {
          if (ctx.caller_fs != nullptr) {
            ctx.code = ctx.caller_fs->code;
            ctx.caller_fs = ctx.caller_fs->parent;
          } else {
            ctx.code = nullptr;
          }
        }
      }
      auto term = block->GetTerminator();
      for (int s = 0, ns = term->numEdges(); s < ns; s++) {
        auto succ = term->successor(s);
        auto [it, inserted] = block_inline_ctx.insert({succ, ctx});
        if (inserted) {
          bfs.push_back(succ);
        } else {
          JIT_DCHECK(
              block_inline_ctx[succ].caller_fs == ctx.caller_fs &&
                  block_inline_ctx[succ].code == ctx.code,
              "inconsistent inline context");
        }
      }
    }
  }
#endif

  UnorderedMap<const hir::BasicBlock*, TranslatedBlock> bb_map;
  std::vector<const hir::BasicBlock*> translated;
  auto translate_block = [&](const hir::BasicBlock* hir_bb) {
#if defined(CINDER_AARCH64)
    auto it = block_inline_ctx.find(hir_bb);
    const hir::FrameState* entry_fs =
        it != block_inline_ctx.end() ? it->second.caller_fs : nullptr;
    BorrowedRef<PyCodeObject> entry_code =
        it != block_inline_ctx.end() ? it->second.code : nullptr;
    bb_map.emplace(
        hir_bb, TranslateOneBasicBlock(hir_bb, entry_fs, entry_code));
#else
    bb_map.emplace(hir_bb, TranslateOneBasicBlock(hir_bb));
#endif
    translated.emplace_back(hir_bb);
  };

  // Translate all reachable blocks.
  auto hir_entry = GetHIRFunction()->cfg.entry_block;
  translate_block(hir_entry);
  for (size_t i = 0; i < translated.size(); ++i) {
    auto hir_term = translated[i]->GetTerminator();
    for (int succ = 0, num_succs = hir_term->numEdges(); succ < num_succs;
         succ++) {
      auto hir_succ = hir_term->successor(succ);
      if (bb_map.contains(hir_succ)) {
        continue;
      }
      translate_block(hir_succ);
    }
  }

  GenerateExitBlocks();

  // Track the exit block explicitly so the block sorter doesn't have to
  // rely on it being basic_blocks_.back().
  lir_func_->setExitBlock(exit_epilogue_ ? exit_epilogue_ : exit_block_);

  // Connect all successors.
  BasicBlockBuilder phi_bbb{env_, lir_func_};
  frame_setup_block_->addSuccessor(bb_map[hir_entry].first);
  for (auto hir_bb : translated) {
    auto hir_term = hir_bb->GetTerminator();
    auto last_bb = bb_map[hir_bb].last;
    switch (hir_term->opcode()) {
      case Opcode::kBranch: {
        auto branch = static_cast<const Branch*>(hir_term);
        auto target_lir_bb = bb_map[branch->target()].first;
        last_bb->addSuccessor(target_lir_bb);
        break;
      }
      case Opcode::kCondBranch:
      case Opcode::kCondBranchCheckType:
      case Opcode::kCondBranchIterNotDone: {
        auto condbranch = static_cast<const CondBranchBase*>(hir_term);
        auto target_lir_true_bb = bb_map[condbranch->true_bb()].first;
        auto target_lir_false_bb = bb_map[condbranch->false_bb()].first;
        last_bb->addSuccessor(target_lir_true_bb);
        last_bb->addSuccessor(target_lir_false_bb);
        last_bb->getLastInstr()->allocateLabelInput(target_lir_true_bb);
        last_bb->getLastInstr()->allocateLabelInput(target_lir_false_bb);
        break;
      }
      case Opcode::kReturn: {
        last_bb->addSuccessor(exit_block_);
        auto* ret = static_cast<const Return*>(hir_term);
        return_edges_.push_back(
            {last_bb, phi_bbb.getDefInstr(ret->GetOperand(0))});
        break;
      }
      default:
        break;
    }
  }

  // Wire up exit phis.
  auto addPhiInput = [&](Instruction* phi, const ExitEdge& edge) {
    phi->allocateLabelInput(edge.block);
    phi->allocateLinkedInput(edge.value);
  };

  // Connect yield exit blocks to the epilogue.
  BasicBlock* yield_target = exit_epilogue_ ? exit_epilogue_ : exit_block_;
  for (auto& edge : yield_exit_edges_) {
    edge.block->addSuccessor(yield_target);
  }

  // Add resume blocks as phantom successors of their yield blocks.
  // These must be added AFTER the exit epilogue successors above so that
  // the exit epilogue is successors.front() — resolveEdges relies on this
  // ordering to resolve the correct edge.
  JIT_CHECK(
      yield_exit_edges_.size() == resume_blocks_.size(),
      "yield_exit_edges_ and resume_blocks_ must correspond 1:1");
  for (size_t i = 0; i < yield_exit_edges_.size(); i++) {
    yield_exit_edges_[i].block->addSuccessor(resume_blocks_[i]);
  }

  // Populate exit_block_ phi with return values (and yield values if no
  // epilogue).
  for (auto& edge : return_edges_) {
    addPhiInput(exit_phi_, edge);
  }
  if (epilogue_phi_ == nullptr) {
    for (auto& edge : yield_exit_edges_) {
      addPhiInput(exit_phi_, edge);
    }
  }

  // For generators, populate exit_epilogue_ phi with values from both
  // the return path (exit_block_) and yield exit blocks.
  if (epilogue_phi_ != nullptr) {
    epilogue_phi_->allocateLabelInput(exit_block_);
    epilogue_phi_->allocateLinkedInput(exit_phi_);
    for (auto& edge : yield_exit_edges_) {
      addPhiInput(epilogue_phi_, edge);
    }
  }

  resolvePhiOperands(bb_map);

  // For generators, create a placeholder resume entry block. This block
  // dispatches to resume targets via indirect jump (populated post-regalloc
  // by PopulateResumeEntryBlock). Its successors are the resume blocks,
  // which keeps them reachable during sortBasicBlocks.
  // We always create this for generators because the codegen for
  // kStoreGenYieldPoint references gen_resume_entry_label (the label
  // bound to this block).
  if (is_gen_) {
    auto* resume_entry = lir_func_->allocateBasicBlock();
    // Remove from basic_blocks_ immediately — this is a placeholder block
    // that will be populated post-regalloc by PopulateResumeEntryBlock and
    // re-inserted in generateCode() before emission.  Leaving it in the
    // list causes the block sorter to treat it as the exit block and assert
    // because it has successors.
    auto& bbs = lir_func_->basicblocks();
    bbs.erase(std::remove(bbs.begin(), bbs.end(), resume_entry), bbs.end());
    for (auto* rb : resume_blocks_) {
      resume_entry->addSuccessor(rb);
    }
    lir_func_->setResumeEntryBlock(resume_entry);
  }

  return function;
}

void LIRGenerator::appendGuardAlwaysFail(
    BasicBlockBuilder& bbb,
    const hir::DeoptBase& hir_instr) {
  auto deopt_id = bbb.makeDeoptMetadata();
  Instruction* instr = bbb.appendInstr(
      Instruction::kGuard,
      Imm{InstrGuardKind::kAlwaysFail},
      Imm{deopt_id},
      Imm{0},
      Imm{0});
  addLiveRegOperands(bbb, instr, hir_instr);
}

void LIRGenerator::addLiveRegOperands(
    BasicBlockBuilder& bbb,
    Instruction* instr,
    const hir::DeoptBase& hir_instr) {
  auto& regstates = hir_instr.live_regs();
  for (const auto& reg_state : regstates) {
    hir::Register* reg = reg_state.reg;
    instr->addOperands(VReg{bbb.getDefInstr(reg)});
  }
}

// Attempt to emit a type-specialized call, returning true if successful.
bool LIRGenerator::TranslateSpecializedCall(
    BasicBlockBuilder& bbb,
    const hir::VectorCall& hir_instr) {
  if (hir_instr.flags() & CallFlags::KwArgs) {
    return false;
  }

  hir::Register* callable = hir_instr.func();
  if (!callable->type().hasValueSpec(TObject)) {
    return false;
  }
  auto callee = callable->type().objectSpec();
  auto type = Py_TYPE(callee);
  if (PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE) ||
      PyType_IsSubtype(type, &PyModule_Type)) {
    // Heap types and ModuleType subtypes support __class__ reassignment, so we
    // can't rely on the object's type.
    return false;
  }

  // Only inline loading the entry points to native functions.  These objects
  // will not have their vectorcall entry points modified by the JIT, so it
  // always makes sense to load them at JIT-time and burn them directly into
  // code.
  if (type != &PyCFunction_Type) {
    return false;
  }

  if (callee == cinderx::getModuleState()->builtin_next) {
    if (hir_instr.numArgs() == 1) {
      bbb.appendCallInstruction(
          hir_instr.output(), Ci_Builtin_Next_Core, hir_instr.arg(0), nullptr);
      return true;
    } else if (hir_instr.numArgs() == 2) {
      bbb.appendCallInstruction(
          hir_instr.output(),
          Ci_Builtin_Next_Core,
          hir_instr.arg(0),
          hir_instr.arg(1));
      return true;
    }
  }

  // This is where we can go bananas with specializing calls to things like
  // tuple(), list(), etc, hardcoding or inlining calls to tp_new and tp_init as
  // appropriate. For now, we simply support any native callable with a
  // vectorcall.
  switch (
      PyCFunction_GET_FLAGS(callee) &
      (METH_VARARGS | METH_FASTCALL | METH_NOARGS | METH_O | METH_KEYWORDS)) {
    case METH_NOARGS:
      if (hir_instr.numArgs() == 0) {
        bbb.appendCallInstruction(
            hir_instr.output(),
            PyCFunction_GET_FUNCTION(callee),
            PyCFunction_GET_SELF(callee),
            nullptr);
        return true;
      }
      break;
    case METH_O:
      if (hir_instr.numArgs() == 1) {
        bbb.appendCallInstruction(
            hir_instr.output(),
            PyCFunction_GET_FUNCTION(callee),
            PyCFunction_GET_SELF(callee),
            hir_instr.arg(0));
        return true;
      }
      break;
  }

  return false;
}

void LIRGenerator::emitExceptionCheck(
    const jit::hir::DeoptBase& i,
    jit::lir::BasicBlockBuilder& bbb) {
  hir::Register* out = i.output();
  if (out->isA(TBottom)) {
    appendGuardAlwaysFail(bbb, i);
  } else {
    auto kind = out->isA(TCSigned) ? InstrGuardKind::kNotNegative
                                   : InstrGuardKind::kNotZero;
    appendGuard(bbb, kind, i, bbb.getDefInstr(out));
  }
}

void LIRGenerator::makeIncref(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    bool xincref,
    [[maybe_unused]] bool possible_immortal) {
  auto end_incref = bbb.allocateBlock();
  if (xincref) {
    auto cont = bbb.allocateBlock();
    bbb.appendBranch(Instruction::kCondBranch, instr, cont, end_incref);
    bbb.appendBlock(cont);
  }

#ifdef Py_GIL_DISABLED
  makeIncrefFreeThreaded(bbb, instr, end_incref);
#else
  makeIncrefGILEnabled(bbb, instr, end_incref, possible_immortal);
#endif

  bbb.appendBlock(end_incref);
}

#ifdef Py_GIL_DISABLED
void LIRGenerator::makeIncrefFreeThreaded(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    BasicBlock* end_incref) {
  // Inline the common-case incref for free-threading. Check thread ownership
  // (ob_tid == tstate->thread_id) and use a relaxed atomic store for
  // thread-owned objects. Fall back to Py_IncRef for objects owned by other
  // threads.
  BasicBlock* slow_incref = bbb.allocateBlock();

  // Load ob_ref_local (32-bit thread-local refcount) with relaxed semantics.
  Instruction* ref_local = bbb.appendInstr(
      OutVReg{OperandBase::k32bit},
      Instruction::kMoveRelaxed,
      Ind{instr,
          static_cast<int>(offsetof(PyObject, ob_ref_local)),
          DataType::k32bit});

  // Increment. If result overflows to 0, ob_ref_local was UINT32_MAX
  // (immortal sentinel) — skip.
  bbb.appendInstr(Instruction::kInc, ref_local);
  bbb.appendBranch(Instruction::kBranchE, end_incref);

  // Check thread ownership: ob_tid vs tstate->thread_id.
  BasicBlock* check_owner = bbb.allocateBlock();
  bbb.appendBlock(check_owner);
  Instruction* ob_tid = bbb.appendInstr(
      OutVReg{},
      Instruction::kMoveRelaxed,
      Ind{instr, static_cast<int>(offsetof(PyObject, ob_tid))});
  Instruction* thread_id = bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      Ind{env_->asm_tstate,
          static_cast<int>(offsetof(PyThreadState, thread_id))});
  bbb.appendInstr(Instruction::kCmp, ob_tid, thread_id);
  bbb.appendBranch(Instruction::kBranchNE, slow_incref);

  // Fast path: thread-owned, store incremented ob_ref_local with relaxed
  // semantics.
  BasicBlock* fast_store = bbb.allocateBlock();
  bbb.appendBlock(fast_store);
  bbb.appendInstr(
      OutInd{
          instr,
          static_cast<int>(offsetof(PyObject, ob_ref_local)),
          DataType::k32bit},
      Instruction::kMoveRelaxed,
      ref_local);
  updateRefTotal(bbb, Instruction::kInc);
  // Jump past the slow path to end_incref.
  bbb.appendBranch(Instruction::kBranch, end_incref);

  // Slow path: not owned by this thread, use atomic Py_IncRef.
  // Use switchBlock (not appendBlock) because fast_store's unconditional
  // branch means slow_incref is not its fallthrough successor.
  bbb.switchBlock(slow_incref);
  if (getConfig().multiple_code_sections) {
    slow_incref->setSection(codegen::CodeSection::kCold);
  }
  bbb.appendInvokeInstruction(Py_IncRef, instr);
  // Falls through to end_incref (appended by caller).
}
#else
void LIRGenerator::makeIncrefGILEnabled(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    BasicBlock* end_incref,
    bool possible_immortal) {
  // If this could be an immortal object then we need to load the refcount as a
  // 32-bit integer to see if it overflows on increment, indicating that it's
  // immortal.  For mortal objects the refcount is a regular 64-bit integer.
  if (possible_immortal) {
    auto mortal = bbb.allocateBlock();
    Instruction* r1 = bbb.appendInstr(
        OutVReg{OperandBase::k32bit},
        Instruction::kMove,
        Ind{instr, kRefcountOffset, DataType::k32bit});
    bbb.appendInstr(Instruction::kInc, r1);
#if PY_VERSION_HEX >= 0x030E0000
    bbb.appendBranch(Instruction::kBranchS, end_incref);
#else
    bbb.appendBranch(Instruction::kBranchE, end_incref);
#endif
    bbb.appendBlock(mortal);
    bbb.appendInstr(
        OutInd{instr, kRefcountOffset, DataType::k32bit},
        Instruction::kMove,
        r1);
  } else {
    Instruction* r1 = bbb.appendInstr(
        OutVReg{}, Instruction::kMove, Ind{instr, kRefcountOffset});
    bbb.appendInstr(Instruction::kInc, r1);
    bbb.appendInstr(OutInd{instr, kRefcountOffset}, Instruction::kMove, r1);
  }

  updateRefTotal(bbb, Instruction::kInc);
}
#endif

void LIRGenerator::makeIncref(
    BasicBlockBuilder& bbb,
    const hir::Instr& instr,
    bool xincref) {
  Register* obj = instr.GetOperand(0);

  // Don't generate anything for immortal objects.
  if (!obj->type().couldBe(TMortalObject)) {
    return;
  }

  makeIncref(
      bbb, bbb.getDefInstr(obj), xincref, obj->type().couldBe(TImmortalObject));
}

void LIRGenerator::makeDecref(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    [[maybe_unused]] std::optional<destructor> destructor,
    bool xdecref,
    [[maybe_unused]] bool possible_immortal) {
  auto end_decref = bbb.allocateBlock();
  if (xdecref) {
    auto cont = bbb.allocateBlock();
    bbb.appendBranch(Instruction::kCondBranch, instr, cont, end_decref);
    bbb.appendBlock(cont);
  }

#ifdef Py_GIL_DISABLED
  makeDecrefFreeThreaded(bbb, instr, end_decref);
#else
  makeDecrefGILEnabled(bbb, instr, end_decref, destructor, possible_immortal);
#endif

  bbb.appendBlock(end_decref);
}

#ifdef Py_GIL_DISABLED
void LIRGenerator::makeDecrefFreeThreaded(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    BasicBlock* end_decref) {
  // Inline the common-case decref for free-threading. Check thread ownership
  // and use a relaxed atomic store for thread-owned objects. When the local
  // refcount reaches zero, call _Py_MergeZeroLocalRefcount to merge with the
  // shared refcount. Fall back to Py_DecRef for objects owned by other threads.
  BasicBlock* slow_decref = bbb.allocateBlock();
  BasicBlock* merge_refcount = bbb.allocateBlock();

  // Load ob_ref_local (32-bit) with relaxed semantics.
  Instruction* ref_local = bbb.appendInstr(
      OutVReg{OperandBase::k32bit},
      Instruction::kMoveRelaxed,
      Ind{instr,
          static_cast<int>(offsetof(PyObject, ob_ref_local)),
          DataType::k32bit});

  // Check immortal via sign bit. Normal refcounts are well below 2^31,
  // so a set sign bit indicates an immortal sentinel.
  bbb.appendInstr(Instruction::kTest32, ref_local, ref_local);
  bbb.appendBranch(Instruction::kBranchS, end_decref);

  // Check thread ownership: ob_tid vs tstate->thread_id.
  BasicBlock* check_owner = bbb.allocateBlock();
  bbb.appendBlock(check_owner);
  Instruction* ob_tid = bbb.appendInstr(
      OutVReg{},
      Instruction::kMoveRelaxed,
      Ind{instr, static_cast<int>(offsetof(PyObject, ob_tid))});
  Instruction* thread_id = bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      Ind{env_->asm_tstate,
          static_cast<int>(offsetof(PyThreadState, thread_id))});
  bbb.appendInstr(Instruction::kCmp, ob_tid, thread_id);
  bbb.appendBranch(Instruction::kBranchNE, slow_decref);

  // Fast path: thread-owned, decrement and store with relaxed semantics.
  BasicBlock* fast_dec = bbb.allocateBlock();
  bbb.appendBlock(fast_dec);
  updateRefTotal(bbb, Instruction::kDec);
  bbb.appendInstr(Instruction::kDec, ref_local);
  bbb.appendInstr(
      OutInd{
          instr,
          static_cast<int>(offsetof(PyObject, ob_ref_local)),
          DataType::k32bit},
      Instruction::kMoveRelaxed,
      ref_local);
  // Re-test zero flag after the store (the store may clobber flags).
  bbb.appendInstr(Instruction::kTest32, ref_local, ref_local);
  // If non-zero, done — branch to end. Zero falls through to merge.
  bbb.appendBranch(Instruction::kBranchNZ, end_decref);

  // Local refcount reached zero — merge with shared refcount. This may
  // deallocate the object if the shared refcount is also zero.
  bbb.appendBlock(merge_refcount);
  if (getConfig().multiple_code_sections) {
    merge_refcount->setSection(codegen::CodeSection::kCold);
  }
  bbb.appendInvokeInstruction(_Py_MergeZeroLocalRefcount, instr);
  // Jump past the slow path to end_decref.
  bbb.appendBranch(Instruction::kBranch, end_decref);

  // Slow path: not owned by this thread, use atomic Py_DecRef.
  // Use switchBlock (not appendBlock) because merge_refcount's
  // unconditional branch means slow_decref is not its fallthrough.
  bbb.switchBlock(slow_decref);
  if (getConfig().multiple_code_sections) {
    slow_decref->setSection(codegen::CodeSection::kCold);
  }
  bbb.appendInvokeInstruction(Py_DecRef, instr);
  // Falls through to end_decref (appended by caller).
}
#else
void LIRGenerator::makeDecrefGILEnabled(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    BasicBlock* end_decref,
    std::optional<destructor> destructor,
    bool possible_immortal) {
  Instruction* r1 = bbb.appendInstr(
      OutVReg{}, Instruction::kMove, Ind{instr, kRefcountOffset});

  if (possible_immortal) {
    auto mortal = bbb.allocateBlock();
    bbb.appendInstr(Instruction::kTest32, r1, r1);
    bbb.appendBranch(Instruction::kBranchS, end_decref);
    bbb.appendBlock(mortal);
  }

  updateRefTotal(bbb, Instruction::kDec);

  auto dealloc = bbb.allocateBlock();
  bbb.appendInstr(Instruction::kDec, r1);
  bbb.appendInstr(OutInd{instr, kRefcountOffset}, Instruction::kMove, r1);
  bbb.appendBranch(Instruction::kBranchNZ, end_decref);
  bbb.appendBlock(dealloc);
  if (getConfig().multiple_code_sections) {
    dealloc->setSection(codegen::CodeSection::kCold);
  }

  if (destructor.has_value()) {
#ifdef Py_TRACE_REFS
    bbb.appendInvokeInstruction(_Py_ForgetReference, obj);
#endif

    bbb.appendInvokeInstruction(destructor.value(), instr);
  } else {
    bbb.appendInvokeInstruction(_Py_Dealloc, instr);
  }
}
#endif

void LIRGenerator::makeDecref(
    BasicBlockBuilder& bbb,
    const jit::hir::Instr& instr,
    bool xdecref) {
  Register* obj = instr.GetOperand(0);

  // Don't generate anything for immortal objects.
  if (!obj->type().couldBe(TMortalObject)) {
    return;
  }

  makeDecref(
      bbb,
      bbb.getDefInstr(obj),
      obj->type().runtimePyTypeDestructor(),
      xdecref,
      obj->type().couldBe(TImmortalObject));
}

LIRGenerator::TranslatedBlock LIRGenerator::TranslateOneBasicBlock(
    const hir::BasicBlock* hir_bb,
    const jit::hir::FrameState* initial_caller_fs,
    BorrowedRef<PyCodeObject> initial_inlined_code) {
  BasicBlockBuilder bbb{env_, lir_func_};
  BasicBlock* entry_block = bbb.allocateBlock();
  bbb.switchBlock(entry_block);

#if defined(CINDER_AARCH64)
  int last_deopt_line = -1;
  const jit::hir::FrameState* caller_fs = initial_caller_fs;
  BorrowedRef<PyCodeObject> inlined_code = initial_inlined_code;
#endif

  for (auto& i : *hir_bb) {
    auto opcode = i.opcode();
    bbb.setCurrentInstr(&i);

#if defined(CINDER_AARCH64)
    if (opcode == Opcode::kBeginInlinedFunction) {
      auto bif = static_cast<const BeginInlinedFunction*>(&i);
      caller_fs = bif->callerFrameState();
      inlined_code = bif->code();
      last_deopt_line = -1;
    } else if (opcode == Opcode::kEndInlinedFunction) {
      if (caller_fs != nullptr) {
        inlined_code = caller_fs->code;
        caller_fs = caller_fs->parent;
      } else {
        inlined_code = nullptr;
      }
      last_deopt_line = -1;
    }
    updateDeoptIndex(bbb, i, opcode, last_deopt_line, caller_fs, inlined_code);
#endif

    switch (opcode) {
      case Opcode::kLoadArg: {
        auto instr = static_cast<const LoadArg*>(&i);
        if (instr->arg_idx() < env_->arg_locations.size() &&
            env_->arg_locations[instr->arg_idx()] != PhyLocation::REG_INVALID) {
          bbb.appendInstr(
              instr->output(), Instruction::kLoadArg, Imm{instr->arg_idx()});
          break;
        }
        size_t reg_count = env_->arg_locations.size();
        for (auto loc : env_->arg_locations) {
          if (loc == PhyLocation::REG_INVALID) {
            reg_count--;
          }
        }
        Instruction* extra_args = env_->asm_extra_args;
        int32_t offset = (instr->arg_idx() - reg_count) * kPointerSize;
        bbb.appendInstr(
            instr->output(), Instruction::kMove, Ind{extra_args, offset});
        break;
      }
      case Opcode::kLoadCurrentFunc: {
        hir::Register* dest = i.output();
        Instruction* func = env_->asm_func;
        bbb.appendInstr(dest, Instruction::kMove, func);
        break;
      }
      case Opcode::kLoadFrame: {
        emitLoadFrame(bbb);
        break;
      }
      case Opcode::kMakeCell: {
        auto instr = static_cast<const MakeCell*>(&i);
        bbb.appendCallInstruction(
            instr->output(), PyCell_New, instr->GetOperand(0));
        break;
      }
      case Opcode::kStealCellItem: {
        hir::Register* dest = i.output();
        Instruction* src_base = bbb.getDefInstr(i.GetOperand(0));
        constexpr int32_t kOffset = offsetof(PyCellObject, ob_ref);
        bbb.appendInstr(dest, Instruction::kMove, Ind{src_base, kOffset});
        break;
      }
      case Opcode::kSwapCellItem: {
#if PY_VERSION_HEX >= 0x030D0000
        // Atomically swap cell value, returning old value for decref.
        // Used in FT-Python for thread-safe STORE_DEREF.
        auto* instr = static_cast<const SwapCellItem*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_SwapCellItem,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
#else
        JIT_ABORT("SwapCellItem requires Python 3.13+");
#endif
      }
      case Opcode::kLoadCellItem: {
        // FT needs to call PyCell_GetRef for thread-safety. Switching the two
        // implementations changes whether the output is borrowed or new so
        // there is a corresponding switch in instr_effects.cpp.
#ifdef Py_GIL_DISABLED
        auto* instr = static_cast<const LoadCellItem*>(&i);
        bbb.appendCallInstruction(
            instr->output(), JITRT_LoadCellItem, instr->GetOperand(0));
#else
        hir::Register* dest = i.output();
        Instruction* src_base = bbb.getDefInstr(i.GetOperand(0));
        constexpr int32_t kOffset = offsetof(PyCellObject, ob_ref);
        bbb.appendInstr(dest, Instruction::kMove, Ind{src_base, kOffset});
#endif
        break;
      }
      case Opcode::kSetCellItem: {
        auto instr = static_cast<const SetCellItem*>(&i);
        bbb.appendInstr(
            OutInd{
                bbb.getDefInstr(instr->GetOperand(0)),
                int32_t{offsetof(PyCellObject, ob_ref)}},
            Instruction::kMove,
            instr->GetOperand(1));
        break;
      }
      case Opcode::kInitFrameCellVars: {
        auto& hir_instr = static_cast<const InitFrameCellVars&>(i);
        bbb.appendInvokeInstruction(
            JITRT_InitFrameCellVars,
            bbb.getDefInstr(hir_instr.func()),
            hir_instr.num_cell_vars(),
            env_->asm_tstate);
        break;
      }
      case Opcode::kLoadConst: {
        auto instr = static_cast<const LoadConst*>(&i);
        Type ty = instr->type();

        if (ty <= TCDouble) {
          // Loads the bits of the double constant into an integer register.
          auto spec_value = bit_cast<uint64_t>(ty.doubleSpec());
          Instruction* double_bits = bbb.appendInstr(
              Instruction::kMove,
              OutVReg{OperandBase::k64bit},
              Imm{spec_value});
          // Moves the value into a floating point register.
          bbb.appendInstr(instr->output(), Instruction::kMove, double_bits);
          break;
        }

        intptr_t spec_value = ty.hasIntSpec()
            ? ty.intSpec()
            : reinterpret_cast<intptr_t>(ty.asObject());
        bbb.appendInstr(
            instr->output(),
            Instruction::kMove,
            // Could be integral or pointer, keep as kObject for now.
            Imm{static_cast<uint64_t>(spec_value), OperandBase::kObject});
        break;
      }
      case Opcode::kLoadVarObjectSize: {
        hir::Register* dest = i.output();
        Instruction* src_base = bbb.getDefInstr(i.GetOperand(0));
        constexpr int32_t kOffset = offsetof(PyVarObject, ob_size);
        bbb.appendInstr(dest, Instruction::kMove, Ind{src_base, kOffset});
        break;
      }
      case Opcode::kLoadFunctionIndirect: {
        // format will pass this down as a constant
        auto instr = static_cast<const LoadFunctionIndirect*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_LoadFunctionIndirect,
            instr->funcptr(),
            instr->descr());
        break;
      }
      case Opcode::kPrimitiveConvert: {
        auto instr = static_cast<const PrimitiveConvert*>(&i);
        if (instr->type() <= TCBool) {
          bbb.appendInstr(instr->output(), Instruction::kMove, instr->src());
        } else if (instr->type() <= TCDouble) {
          bbb.appendInstr(
              instr->output(), Instruction::kInt64ToDouble, instr->src());
        } else if (instr->type() <= TCUnsigned) {
          bbb.appendInstr(instr->output(), Instruction::kZext, instr->src());
        } else {
          JIT_CHECK(
              instr->type() <= TCSigned,
              "Unexpected PrimitiveConvert type {}",
              instr->type());
          bbb.appendInstr(instr->output(), Instruction::kSext, instr->src());
        }
        break;
      }
      case Opcode::kIntBinaryOp: {
        auto instr = static_cast<const IntBinaryOp*>(&i);
        auto op = Instruction::kNop;
        std::optional<Instruction::Opcode> extend;
        uint64_t helper = 0;
        switch (instr->op()) {
          case BinaryOpKind::kAdd:
            op = Instruction::kAdd;
            break;
          case BinaryOpKind::kAnd:
            op = Instruction::kAnd;
            break;
          case BinaryOpKind::kSubtract:
            op = Instruction::kSub;
            break;
          case BinaryOpKind::kXor:
            op = Instruction::kXor;
            break;
          case BinaryOpKind::kOr:
            op = Instruction::kOr;
            break;
          case BinaryOpKind::kMultiply:
            op = Instruction::kMul;
            break;
          case BinaryOpKind::kLShift:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kSext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftLeft32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftLeft64);
                break;
            }
            break;
          case BinaryOpKind::kRShift:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kSext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftRight32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftRight64);
                break;
            }
            break;
          case BinaryOpKind::kRShiftUnsigned:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kZext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftRightUnsigned32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_ShiftRightUnsigned64);
                break;
            }
            break;
          case BinaryOpKind::kFloorDivide:
            op = Instruction::kDiv;
            break;
          case BinaryOpKind::kFloorDivideUnsigned:
            op = Instruction::kDivUn;
            break;
          case BinaryOpKind::kModulo:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kSext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_Mod32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_Mod64);
                break;
            }
            break;
          case BinaryOpKind::kModuloUnsigned:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kZext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_ModUnsigned32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_ModUnsigned64);
                break;
            }
            break;
          case BinaryOpKind::kPower:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kSext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_Power32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_Power64);
                break;
            };
            break;
          case BinaryOpKind::kPowerUnsigned:
            switch (bytes_from_cint_type(instr->GetOperand(0)->type())) {
              case 1:
              case 2:
                extend = Instruction::kZext;
                [[fallthrough]];
              case 3:
                helper = reinterpret_cast<uint64_t>(JITRT_PowerUnsigned32);
                break;
              case 4:
                helper = reinterpret_cast<uint64_t>(JITRT_PowerUnsigned64);
                break;
            };
            break;
          default:
            JIT_ABORT("not implemented");
        }

        // Verify CBools.
        if (instr->left()->isA(TCBool) && instr->right()->isA(TCBool)) {
          switch (instr->op()) {
            case BinaryOpKind::kAnd:
            case BinaryOpKind::kOr:
            case BinaryOpKind::kXor:
              break;
            default:
              JIT_ABORT(
                  "Unrecognized binary op {} for bool IntBinaryOp",
                  GetBinaryOpName(instr->op()));
          }
        }

        if (helper != 0) {
          Instruction* left = bbb.getDefInstr(instr->left());
          Instruction* right = bbb.getDefInstr(instr->right());
          if (extend.has_value()) {
            auto dt = OperandBase::k32bit;
            left = bbb.appendInstr(*extend, OutVReg{dt}, left);
            right = bbb.appendInstr(*extend, OutVReg{dt}, right);
          }
          bbb.appendInstr(
              instr->output(),
              Instruction::kCall,
              // TASK(T140174965): This should be MemImm.
              Imm{reinterpret_cast<uint64_t>(helper)},
              left,
              right);
        } else if (
            instr->op() == BinaryOpKind::kFloorDivide ||
            instr->op() == BinaryOpKind::kFloorDivideUnsigned) {
          // Divides take an extra zero argument.
          bbb.appendInstr(
              instr->output(), op, Imm{0}, instr->left(), instr->right());
        } else {
          bbb.appendInstr(instr->output(), op, instr->left(), instr->right());
        }

        break;
      }
      case Opcode::kDoubleBinaryOp: {
        auto instr = static_cast<const DoubleBinaryOp*>(&i);

        if (instr->op() == BinaryOpKind::kPower) {
          Type right_type = instr->right()->type();
          if (right_type.hasDoubleSpec() && right_type.doubleSpec() == 0.5) {
            bbb.appendCallInstruction(
                instr->output(), JITRT_SqrtDouble, instr->left());
          } else {
            bbb.appendCallInstruction(
                instr->output(),
                JITRT_PowerDouble,
                instr->left(),
                instr->right());
          }
          break;
        }

        auto op = Instruction::kNop;
        switch (instr->op()) {
          case BinaryOpKind::kAdd: {
            op = Instruction::kFadd;
            break;
          }
          case BinaryOpKind::kSubtract: {
            op = Instruction::kFsub;
            break;
          }
          case BinaryOpKind::kMultiply: {
            op = Instruction::kFmul;
            break;
          }
          case BinaryOpKind::kTrueDivide: {
            op = Instruction::kFdiv;
            break;
          }
          default: {
            JIT_ABORT("Invalid operation for DoubleBinaryOp");
          }
        }

        bbb.appendInstr(instr->output(), op, instr->left(), instr->right());
        break;
      }
      case Opcode::kPrimitiveCompare: {
        auto instr = static_cast<const PrimitiveCompare*>(&i);
        Instruction::Opcode op;
        switch (instr->op()) {
          case PrimitiveCompareOp::kEqual:
            op = Instruction::kEqual;
            break;
          case PrimitiveCompareOp::kNotEqual:
            op = Instruction::kNotEqual;
            break;
          case PrimitiveCompareOp::kGreaterThanUnsigned:
            op = Instruction::kGreaterThanUnsigned;
            break;
          case PrimitiveCompareOp::kGreaterThan:
            op = Instruction::kGreaterThanSigned;
            break;
          case PrimitiveCompareOp::kLessThanUnsigned:
            op = Instruction::kLessThanUnsigned;
            break;
          case PrimitiveCompareOp::kLessThan:
            op = Instruction::kLessThanSigned;
            break;
          case PrimitiveCompareOp::kGreaterThanEqualUnsigned:
            op = Instruction::kGreaterThanEqualUnsigned;
            break;
          case PrimitiveCompareOp::kGreaterThanEqual:
            op = Instruction::kGreaterThanEqualSigned;
            break;
          case PrimitiveCompareOp::kLessThanEqualUnsigned:
            op = Instruction::kLessThanEqualUnsigned;
            break;
          case PrimitiveCompareOp::kLessThanEqual:
            op = Instruction::kLessThanEqualSigned;
            break;
          default:
            JIT_ABORT("Not implemented {}", static_cast<int>(instr->op()));
        }
        bbb.appendInstr(instr->output(), op, instr->left(), instr->right());
        break;
      }
      case Opcode::kPrimitiveBoxBool: {
        // Boxing a boolean is a matter of selecting between Py_True and
        // Py_False.
        Register* dest = i.output();
        Register* src = i.GetOperand(0);
        auto true_addr = reinterpret_cast<uint64_t>(Py_True);
        auto false_addr = reinterpret_cast<uint64_t>(Py_False);
        Instruction* temp_true = bbb.appendInstr(
            Instruction::kMove, OutVReg{OperandBase::k64bit}, Imm{true_addr});
        bbb.appendInstr(
            dest, Instruction::kSelect, src, temp_true, Imm{false_addr});
        break;
      }
      case Opcode::kPrimitiveBox: {
        auto instr = static_cast<const PrimitiveBox*>(&i);
        Instruction* src = bbb.getDefInstr(instr->value());
        Type src_type = instr->value()->type();
        uint64_t func = 0;

        if (src_type == TNullptr) {
          // special case for an uninitialized variable, we'll
          // load zero
          bbb.appendCallInstruction(instr->output(), JITRT_BoxI64, int64_t{0});
          break;
        } else if (src_type <= TCUInt64) {
          func = reinterpret_cast<uint64_t>(JITRT_BoxU64);
        } else if (src_type <= TCInt64) {
          func = reinterpret_cast<uint64_t>(JITRT_BoxI64);
        } else if (src_type <= TCUInt32) {
          func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
        } else if (src_type <= TCInt32) {
          func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
        } else if (src_type <= TCDouble) {
          func = reinterpret_cast<uint64_t>(JITRT_BoxDouble);
        } else if (src_type <= (TCUInt8 | TCUInt16)) {
          src = bbb.appendInstr(
              Instruction::kZext, OutVReg{OperandBase::k32bit}, src);
          func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
        } else if (src_type <= (TCInt8 | TCInt16)) {
          src = bbb.appendInstr(
              Instruction::kSext, OutVReg{OperandBase::k32bit}, src);
          func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
        }

        JIT_CHECK(func != 0, "Unknown box type {}", src_type.toString());

        bbb.appendInstr(
            instr->output(),
            Instruction::kCall,
            // TASK(T140174965): This should be MemImm.
            Imm{func},
            src);

        break;
      }

      case Opcode::kIsNegativeAndErrOccurred: {
        // Emit code to do the following:
        //   dst = (src == -1 && tstate->current_exception != nullptr) ? -1 : 0;

        auto instr = static_cast<const IsNegativeAndErrOccurred*>(&i);
        Type src_type = instr->reg()->type();

        // We do have to widen to at least 32 bits due to calling convention
        // always passing a minimum of 32 bits.
        Instruction* src = bbb.getDefInstr(instr->reg());
        if (src_type <= (TCBool | TCInt8 | TCUInt8 | TCInt16 | TCUInt16)) {
          src = bbb.appendInstr(
              Instruction::kSext, OutVReg{OperandBase::k32bit}, src);
        }

        // Because a failed unbox to unsigned smuggles the bit pattern for a
        // signed -1 in the unsigned value, we can likewise just treat unsigned
        // as signed for purposes of checking for -1 here.
        Instruction* is_not_negative = bbb.appendInstr(
            Instruction::kNotEqual,
            OutVReg{DataType::k8bit},
            src,
            Imm{static_cast<uint64_t>(-1), src->output()->dataType()});

        bbb.appendInstr(instr->output(), Instruction::kMove, Imm{0});

        auto check_err = bbb.allocateBlock();
        auto set_err = bbb.allocateBlock();
        auto done = bbb.allocateBlock();

        bbb.appendBranch(
            Instruction::kCondBranch, is_not_negative, done, check_err);
        bbb.switchBlock(check_err);

        constexpr int32_t kOffset = offsetof(PyThreadState, current_exception);
        Instruction* curexc = bbb.appendInstr(
            Instruction::kMove, OutVReg{}, Ind{env_->asm_tstate, kOffset});

        Instruction* is_no_err_set = bbb.appendInstr(
            Instruction::kEqual, OutVReg{OperandBase::k8bit}, curexc, Imm{0});

        bbb.appendBranch(
            Instruction::kCondBranch, is_no_err_set, done, set_err);
        bbb.switchBlock(set_err);

        // Set to -1 in the error case.
        bbb.appendInstr(Instruction::kDec, instr->output());
        bbb.switchBlock(done);
        break;
      }

      case Opcode::kPrimitiveUnbox: {
        auto instr = static_cast<const PrimitiveUnbox*>(&i);
        Type ty = instr->type();
        if (ty <= TCBool) {
          bbb.appendInstr(
              instr->output(),
              Instruction::kEqual,
              instr->value(),
              Imm{reinterpret_cast<uint64_t>(Py_True), OperandBase::kObject});
        } else if (ty <= TCDouble) {
          // For doubles, we can directly load the offset into the destination.
          Instruction* value = bbb.getDefInstr(instr->value());
          int32_t offset = offsetof(PyFloatObject, ob_fval);
          bbb.appendInstr(
              instr->output(), Instruction::kMove, Ind{value, offset});
        } else if (ty <= TCUInt64) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxU64, instr->value());
        } else if (ty <= TCUInt32) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxU32, instr->value());
        } else if (ty <= TCUInt16) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxU16, instr->value());
        } else if (ty <= TCUInt8) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxU8, instr->value());
        } else if (ty <= TCInt64) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxI64, instr->value());
        } else if (ty <= TCInt32) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxI32, instr->value());
        } else if (ty <= TCInt16) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxI16, instr->value());
        } else if (ty <= TCInt8) {
          bbb.appendCallInstruction(
              instr->output(), JITRT_UnboxI8, instr->value());
        } else {
          JIT_ABORT("Cannot unbox type {}", ty.toString());
        }
        break;
      }
      case Opcode::kIndexUnbox: {
        auto instr = static_cast<const IndexUnbox*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            PyNumber_AsSsize_t,
            instr->GetOperand(0),
            instr->exception());
        break;
      }
      case Opcode::kPrimitiveUnaryOp: {
        auto instr = static_cast<const PrimitiveUnaryOp*>(&i);
        switch (instr->op()) {
          case PrimitiveUnaryOpKind::kNegateInt:
            bbb.appendInstr(
                instr->output(), Instruction::kNegate, instr->value());
            break;
          case PrimitiveUnaryOpKind::kInvertInt:
            bbb.appendInstr(
                instr->output(), Instruction::kInvert, instr->value());
            break;
          case PrimitiveUnaryOpKind::kNotInt: {
            bbb.appendInstr(
                instr->output(),
                Instruction::kEqual,
                instr->value(),
                Imm{0, hirTypeToDataType(instr->value()->type())});
            break;
          }
          default:
            JIT_ABORT(
                "Not implemented unary op {}", static_cast<int>(instr->op()));
        }
        break;
      }
      case Opcode::kReturn: {
        bbb.appendInstr(Instruction::kReturn);
        break;
      }
      case Opcode::kSetCurrentAwaiter: {
        bbb.appendInvokeInstruction(
            JITRT_SetCurrentAwaiter, i.GetOperand(0), env_->asm_tstate);
        break;
      }
      case Opcode::kYieldValue: {
        auto hir_instr = static_cast<const YieldValue*>(&i);

        // 1. kStoreGenYieldPoint / kStoreGenYieldFromPoint: store yield point
        //    metadata and live regs.
        Instruction* store_instr;
        if (hir_instr->isYieldFrom()) {
          store_instr = bbb.appendInstr(Instruction::kStoreGenYieldFromPoint);
          // Add the sub-iterator as input 0 so that
          // emitStoreGenYieldPoint can capture its spill offset.
          store_instr->addOperands(
              VReg{bbb.getDefInstr(hir_instr->yieldFromIter())});
        } else {
          store_instr = bbb.appendInstr(Instruction::kStoreGenYieldPoint);
        }
        finishYield(bbb, store_instr, hir_instr);

        // 2. kBranchToYieldExit: terminates this block. The yield value
        //    flows to the exit epilogue via the CFG (phi + edge resolution).
        auto* branch = bbb.appendInstr(Instruction::kBranchToYieldExit);
        yield_exit_edges_.push_back(
            {branch->basicblock(), bbb.getDefInstr(hir_instr->reg())});

        // 3. Split block: resume path starts in a new basic block.
        //    The resume block is added as a successor of the yield block
        //    later in finishFunction (after the exit epilogue successor),
        //    so that the register allocator can propagate liveness across
        //    the yield/resume boundary.
        BasicBlock* resume_block = bbb.allocateBlock();
        resume_blocks_.push_back(resume_block);

        bbb.switchBlock(resume_block);

        // 4. kResumeGenYield: bind resume label, load resumed inputs.
        //    Has output (the sent-in value) and takes tstate as input.
        bbb.appendInstr(
            hir_instr->output(),
            Instruction::kResumeGenYield,
            env_->asm_tstate);
        break;
      }
      case Opcode::kInitialYield: {
        auto hir_instr = static_cast<const InitialYield*>(&i);

        // Unlink the generator frame and get the gen object back.
        Instruction* gen_obj;
        Instruction* footer;
        appendCall2RetValues(
            bbb,
            gen_obj,
            footer,
            JITRT_UnlinkGenFrameAndReturnGenDataFooter,
            env_->asm_tstate);

        // Store yield point metadata (same as kYieldValue).
        Instruction* store = bbb.appendInstr(Instruction::kStoreGenYieldPoint);
        finishYield(bbb, store, hir_instr);

        // kBranchToYieldExit: terminates this block.
        auto* branch = bbb.appendInstr(Instruction::kBranchToYieldExit);
        yield_exit_edges_.push_back({branch->basicblock(), gen_obj});

        // Split block: resume path starts in a new basic block.
        // Split block: resume path starts in a new basic block.
        // Resume successor added in finishFunction (see kYieldValue).
        BasicBlock* resume_block = bbb.allocateBlock();
        resume_blocks_.push_back(resume_block);

        bbb.switchBlock(resume_block);

        // kResumeGenYield: bind resume label, load resumed inputs.
        bbb.appendInstr(
            hir_instr->output(),
            Instruction::kResumeGenYield,
            env_->asm_tstate);
        break;
      }
      case Opcode::kAssign: {
        JIT_CHECK(false, "assign shouldn't be present");
      }
      case Opcode::kBitCast: {
        // BitCasts are purely informative
        break;
      }
      case Opcode::kCondBranch:
      case Opcode::kCondBranchIterNotDone: {
        Instruction* cond = bbb.getDefInstr(i.GetOperand(0));

        if (opcode == Opcode::kCondBranchIterNotDone) {
          auto iter_done_addr =
              reinterpret_cast<uint64_t>(&JITRT_IterDoneSentinel);
          cond = bbb.appendInstr(
              Instruction::kSub,
              OutVReg{OperandBase::k64bit},
              cond,
              Imm{iter_done_addr});
        }

        bbb.appendInstr(Instruction::kCondBranch, cond);
        break;
      }
      case Opcode::kCondBranchCheckType: {
        auto& instr = static_cast<const CondBranchCheckType&>(i);
        auto type = instr.type();
        Instruction* eq_res_var = nullptr;
        if (type.isExact()) {
          Instruction* reg = bbb.getDefInstr(instr.reg());
          constexpr int32_t kOffset = offsetof(PyObject, ob_type);
          Instruction* type_var =
              bbb.appendInstr(Instruction::kMove, OutVReg{}, Ind{reg, kOffset});
          eq_res_var = bbb.appendInstr(
              Instruction::kEqual,
              OutVReg{OperandBase::k8bit},
              type_var,
              Imm{reinterpret_cast<uint64_t>(type.uniquePyType()),
                  DataType::kObject});
        } else {
          eq_res_var = emitSubclassCheck(bbb, instr.GetOperand(0), type);
        }
        bbb.appendInstr(Instruction::kCondBranch, eq_res_var);
        break;
      }
      case Opcode::kDeleteAttr: {
        auto instr = static_cast<const DeleteAttr*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        Instruction* call = bbb.appendInstr(
            Instruction::kCall,
            OutVReg{OperandBase::k32bit},
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(PyObject_SetAttr)},
            instr->GetOperand(0),
            name,
            Imm{0});
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, call);
        break;
      }
      case Opcode::kLoadAttr: {
        auto instr = static_cast<const LoadAttr*>(&i);
        hir::Register* dst = instr->output();
        hir::Register* base = instr->GetOperand(0);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(dst, PyObject_GetAttr, base, name);
        break;
      }
      case Opcode::kLoadAttrCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadAttrCached");
        auto instr = static_cast<const LoadAttrCached*>(&i);
        hir::Register* dst = instr->output();
        hir::Register* base = instr->GetOperand(0);
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache = getContext()->allocateLoadAttrCache();
        bbb.appendCallInstruction(
            dst, jit::LoadAttrCache::invoke, cache, base, name);
        break;
      }
      case Opcode::kLoadAttrSpecial: {
        auto instr = static_cast<const LoadAttrSpecial*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_LookupAttrSpecial,
            instr->GetOperand(0),
            instr->id(),
            instr->failureFmtStr());
        break;
      }
      case Opcode::kLoadTypeAttrCacheEntryType: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadTypeAttrCacheEntryType");
        auto instr = static_cast<const LoadTypeAttrCacheEntryType*>(&i);
        LoadTypeAttrCache* cache = load_type_attr_caches_.at(instr->cache_id());
        PyTypeObject** addr = cache->typeAddr();
        bbb.appendInstr(instr->output(), Instruction::kMove, MemImm{addr});
        break;
      }
      case Opcode::kLoadTypeAttrCacheEntryValue: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadTypeAttrCacheEntryValue");
        auto instr = static_cast<const LoadTypeAttrCacheEntryValue*>(&i);
        LoadTypeAttrCache* cache = load_type_attr_caches_.at(instr->cache_id());
        PyObject** addr = cache->valueAddr();
        bbb.appendInstr(instr->output(), Instruction::kMove, MemImm{addr});
        break;
      }
      case Opcode::kFillTypeAttrCache: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use FillTypeAttrCacheItem");
        auto instr = static_cast<const FillTypeAttrCache*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(
            instr->output(),
            LoadTypeAttrCache::invoke,
            load_type_attr_caches_.at(instr->cache_id()),
            instr->receiver(),
            name);
        break;
      }
      case Opcode::kFillTypeMethodCache: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use FillTypeMethodCache");
        auto instr = static_cast<const FillTypeMethodCache*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache_entry = load_type_method_caches_.at(instr->cache_id());
        if (getConfig().collect_attr_cache_stats) {
          BorrowedRef<PyCodeObject> code = instr->frameState()->code;
          cache_entry->initCacheStats(
              PyUnicode_AsUTF8(code->co_filename),
              PyUnicode_AsUTF8(code->co_name));
        }
        appendCall2RetValues(
            bbb,
            instr->output(),
            LoadTypeMethodCache::lookupHelper,
            cache_entry,
            instr->receiver(),
            name);
        break;
      }
      case Opcode::kLoadTypeMethodCacheEntryType: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use "
            "LoadTypeMethodCacheEntryType");
        auto instr = static_cast<const LoadTypeMethodCacheEntryType*>(&i);
        LoadTypeMethodCache* cache =
            load_type_method_caches_.at(instr->cache_id());
        PyTypeObject** addr = cache->typeAddr();
        bbb.appendInstr(instr->output(), Instruction::kMove, MemImm{addr});
        break;
      }
      case Opcode::kLoadTypeMethodCacheEntryValue: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use "
            "LoadTypeMethodCacheEntryValue");
        auto instr = static_cast<const LoadTypeMethodCacheEntryValue*>(&i);
        LoadTypeMethodCache* cache =
            load_type_method_caches_.at(instr->cache_id());
        appendCall2RetValues(
            bbb,
            instr->output(),
            LoadTypeMethodCache::getValueHelper,
            cache,
            instr->receiver());
        break;
      }
      case Opcode::kLoadMethod: {
        auto instr = static_cast<const LoadMethod*>(&i);
        hir::Register* dst = instr->output();
        hir::Register* base = instr->receiver();
        Instruction* name = getNameFromIdx(bbb, instr);
        appendCall2RetValues(bbb, dst, JITRT_GetMethod, base, name);
        break;
      }
      case Opcode::kLoadMethodCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadMethodCached");
        auto instr = static_cast<const LoadMethodCached*>(&i);
        hir::Register* dst = instr->output();
        hir::Register* base = instr->receiver();
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache = getContext()->allocateLoadMethodCache();
        if (getConfig().collect_attr_cache_stats) {
          BorrowedRef<PyCodeObject> code = instr->frameState()->code;
          cache->initCacheStats(
              PyUnicode_AsUTF8(code->co_filename),
              PyUnicode_AsUTF8(code->co_name));
        }
        appendCall2RetValues(
            bbb, dst, LoadMethodCache::lookupHelper, cache, base, name);
        break;
      }
      case Opcode::kLoadModuleAttrCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadModuleAttrCached");
        auto instr = static_cast<const LoadModuleAttrCached*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache = getContext()->allocateLoadModuleAttrCache();
        bbb.appendCallInstruction(
            instr->output(),
            LoadModuleAttrCache::lookupHelper,
            cache,
            instr->GetOperand(0),
            name);
        break;
      }
      case Opcode::kLoadModuleMethodCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadModuleMethodCached");
        auto instr = static_cast<const LoadModuleMethodCached*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache_entry = getContext()->allocateLoadModuleMethodCache();
        appendCall2RetValues(
            bbb,
            instr->output(),
            LoadModuleMethodCache::lookupHelper,
            cache_entry,
            instr->receiver(),
            name);
        break;
      }
      case Opcode::kGetSecondOutput: {
        bbb.appendInstr(
            i.output(), Instruction::kLoadSecondCallResult, i.GetOperand(0));
        break;
      }
      case Opcode::kLoadMethodSuper: {
        auto instr = static_cast<const LoadMethodSuper*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        appendCall2RetValues(
            bbb,
            instr->output(),
            JITRT_GetMethodFromSuper,
            instr->global_super(),
            instr->type(),
            instr->receiver(),
            name,
            instr->no_args_in_super_call());
        break;
      }
      case Opcode::kLoadAttrSuper: {
        auto instr = static_cast<const LoadAttrSuper*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_GetAttrFromSuper,
            instr->global_super(),
            instr->type(),
            instr->receiver(),
            name,
            instr->no_args_in_super_call());
        break;
      }
      case Opcode::kBinaryOp: {
        auto bin_op = static_cast<const BinaryOp*>(&i);

        // NB: This needs to be in the order that the values appear in the
        // BinaryOpKind enum
        static const binaryfunc helpers[] = {
            PyNumber_Add,
            PyNumber_And,
            PyNumber_FloorDivide,
            PyNumber_Lshift,
            PyNumber_MatrixMultiply,
            PyNumber_Remainder,
            PyNumber_Multiply,
            PyNumber_Or,
            nullptr, // PyNumber_Power is a ternary op.
            PyNumber_Rshift,
            PyObject_GetItem,
            PyNumber_Subtract,
            PyNumber_TrueDivide,
            PyNumber_Xor,
        };
        JIT_CHECK(
            static_cast<unsigned long>(bin_op->op()) < sizeof(helpers),
            "unsupported binop");
        auto op_kind = static_cast<int>(bin_op->op());

        if (bin_op->op() != BinaryOpKind::kPower) {
          bbb.appendCallInstruction(
              bin_op->output(),
              helpers[op_kind],
              bin_op->left(),
              bin_op->right());
        } else {
          bbb.appendCallInstruction(
              bin_op->output(),
              PyNumber_Power,
              bin_op->left(),
              bin_op->right(),
              Py_None);
        }
        break;
      }
      case Opcode::kLongBinaryOp: {
        auto instr = static_cast<const LongBinaryOp*>(&i);
        if (instr->op() == BinaryOpKind::kPower) {
          bbb.appendCallInstruction(
              instr->output(),
              PyLong_Type.tp_as_number->nb_power,
              instr->left(),
              instr->right(),
              Py_None);
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              instr->slotMethod(),
              instr->left(),
              instr->right());
        }
        break;
      }
      case Opcode::kLongInPlaceOp: {
        auto instr = static_cast<const LongInPlaceOp*>(&i);
        if (instr->op() == InPlaceOpKind::kPower) {
          bbb.appendCallInstruction(
              instr->output(),
              PyLong_Type.tp_as_number->nb_power,
              instr->left(),
              instr->right(),
              Py_None);
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              instr->slotMethod(),
              instr->left(),
              instr->right());
        }
        break;
      }
      case Opcode::kFloatBinaryOp: {
        auto instr = static_cast<const FloatBinaryOp*>(&i);
        if (instr->op() == BinaryOpKind::kPower) {
          bbb.appendCallInstruction(
              instr->output(),
              PyFloat_Type.tp_as_number->nb_power,
              instr->left(),
              instr->right(),
              Py_None);
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              instr->slotMethod(),
              instr->left(),
              instr->right());
        }
        break;
      }
      case Opcode::kUnaryOp: {
        auto unary_op = static_cast<const UnaryOp*>(&i);

        // NB: This needs to be in the order that the values appear in the
        // UnaryOpKind enum
        static const unaryfunc helpers[] = {
            JITRT_UnaryNot,
            PyNumber_Negative,
            PyNumber_Positive,
            PyNumber_Invert,
        };
        JIT_CHECK(
            static_cast<unsigned long>(unary_op->op()) < sizeof(helpers),
            "unsupported unaryop");

        auto op_kind = static_cast<int>(unary_op->op());
        bbb.appendCallInstruction(
            unary_op->output(), helpers[op_kind], unary_op->operand());
        break;
      }
      case Opcode::kIsInstance: {
        auto instr = static_cast<const IsInstance*>(&i);
        Instruction* call_instr = bbb.appendCallInstruction(
            instr->output(),
            PyObject_IsInstance,
            instr->GetOperand(0),
            instr->GetOperand(1));
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, call_instr);
        break;
      }
      case Opcode::kCompare: {
        auto instr = static_cast<const Compare*>(&i);
        if (instr->op() == CompareOp::kIn) {
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_SequenceContains,
              instr->right(),
              instr->left());
          break;
        }
        if (instr->op() == CompareOp::kNotIn) {
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_SequenceNotContains,
              instr->right(),
              instr->left());
          break;
        }
        int op = static_cast<int>(instr->op());
        JIT_CHECK(op >= Py_LT, "invalid compare op {}", op);
        JIT_CHECK(op <= Py_GE, "invalid compare op {}", op);
        bbb.appendCallInstruction(
            instr->output(),
            PyObject_RichCompare,
            instr->left(),
            instr->right(),
            op);
        break;
      }
      case Opcode::kFloatCompare: {
        auto instr = static_cast<const FloatCompare*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyFloat_Type.tp_richcompare,
            instr->left(),
            instr->right(),
            static_cast<int>(instr->op()));
        break;
      }
      case Opcode::kLongCompare: {
        auto instr = static_cast<const LongCompare*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyLong_Type.tp_richcompare,
            instr->left(),
            instr->right(),
            static_cast<int>(instr->op()));
        break;
      }
      case Opcode::kUnicodeCompare: {
        auto instr = static_cast<const UnicodeCompare*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyUnicode_Type.tp_richcompare,
            instr->left(),
            instr->right(),
            static_cast<int>(instr->op()));
        break;
      }
      case Opcode::kUnicodeConcat: {
        auto instr = static_cast<const UnicodeConcat*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyUnicode_Concat,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kUnicodeRepeat: {
        auto instr = static_cast<const UnicodeRepeat*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyUnicode_Type.tp_as_sequence->sq_repeat,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kUnicodeSubscr: {
        auto instr = static_cast<const UnicodeSubscr*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PyUnicode_Type.tp_as_sequence->sq_item,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kCompareBool: {
        auto instr = static_cast<const CompareBool*>(&i);
        Instruction* call_instr;
        if (instr->op() == CompareOp::kIn) {
          if (instr->right()->type() <= TUnicodeExact) {
            call_instr = bbb.appendCallInstruction(
                instr->output(),
                PyUnicode_Contains,
                instr->right(),
                instr->left());
          } else {
            call_instr = bbb.appendCallInstruction(
                instr->output(),
                PySequence_Contains,
                instr->right(),
                instr->left());
          }
        } else if (instr->op() == CompareOp::kNotIn) {
          call_instr = bbb.appendCallInstruction(
              instr->output(),
              JITRT_NotContainsBool,
              instr->right(),
              instr->left());
        } else if (
            (instr->op() == CompareOp::kEqual ||
             instr->op() == CompareOp::kNotEqual) &&
            (instr->left()->type() <= TUnicodeExact ||
             instr->right()->type() <= TUnicodeExact)) {
          call_instr = bbb.appendCallInstruction(
              instr->output(),
              JITRT_UnicodeEquals,
              instr->left(),
              instr->right(),
              static_cast<int>(instr->op()));
        } else if (
            (instr->op() == CompareOp::kEqual ||
             instr->op() == CompareOp::kNotEqual) &&
            (isTypeWithReasonablePointerEq(instr->left()->type()) ||
             isTypeWithReasonablePointerEq(instr->right()->type()))) {
          call_instr = bbb.appendCallInstruction(
              instr->output(),
              PyObject_RichCompareBool,
              instr->left(),
              instr->right(),
              static_cast<int>(instr->op()));
        } else {
          call_instr = bbb.appendCallInstruction(
              instr->output(),
              JITRT_RichCompareBool,
              instr->left(),
              instr->right(),
              static_cast<int>(instr->op()));
        }
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, call_instr);
        break;
      }
      case Opcode::kCopyDictWithoutKeys: {
        auto instr = static_cast<const CopyDictWithoutKeys*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_CopyDictWithoutKeys,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kIncref: {
        makeIncref(bbb, i, false);
        break;
      }
      case Opcode::kXIncref: {
        makeIncref(bbb, i, true);
        break;
      }
      case Opcode::kDecref: {
        makeDecref(bbb, i, false);
        break;
      }
      case Opcode::kXDecref: {
        makeDecref(bbb, i, true);
        break;
      }
      case Opcode::kBatchDecref: {
        auto instr = static_cast<const BatchDecref*>(&i);

        Instruction* lir = bbb.appendInstr(Instruction::kVarArgCall);
        lir->addOperands(Imm{reinterpret_cast<uint64_t>(JITRT_BatchDecref)});
        for (hir::Register* arg : instr->GetOperands()) {
          lir->addOperands(VReg{bbb.getDefInstr(arg)});
        }

        break;
      }
      case Opcode::kDeopt: {
        appendGuardAlwaysFail(bbb, static_cast<const DeoptBase&>(i));
        break;
      }
      case Opcode::kUnreachable: {
        bbb.appendInstr(Instruction::kUnreachable);
        break;
      }
      case Opcode::kDeoptPatchpoint: {
        const auto& instr = static_cast<const DeoptPatchpoint&>(i);
        std::size_t deopt_id = bbb.makeDeoptMetadata();
        auto& regstates = instr.live_regs();
        Instruction* lir = bbb.appendInstr(
            Instruction::kDeoptPatchpoint,
            MemImm{instr.patcher()},
            Imm{deopt_id});
        for (const auto& reg_state : regstates) {
          lir->addOperands(VReg{bbb.getDefInstr(reg_state.reg)});
        }
        break;
      }
      case Opcode::kRaiseAwaitableError: {
        const auto& instr = static_cast<const RaiseAwaitableError&>(i);
        bbb.appendInvokeInstruction(
            JITRT_FormatAwaitableError,
            env_->asm_tstate,
            instr.GetOperand(0),
            instr.isAEnter());
        appendGuardAlwaysFail(bbb, instr);
        break;
      }
      case Opcode::kCheckErrOccurred: {
        const auto& instr = static_cast<const DeoptBase&>(i);
        constexpr int32_t kOffset = offsetof(PyThreadState, current_exception);
        Instruction* load = bbb.appendInstr(
            Instruction::kMove, OutVReg{}, Ind{env_->asm_tstate, kOffset});
        appendGuard(bbb, InstrGuardKind::kZero, instr, load);
        break;
      }
      case Opcode::kCheckExc:
      case Opcode::kCheckField:
      case Opcode::kCheckFreevar:
      case Opcode::kCheckNeg:
      case Opcode::kCheckVar:
      case Opcode::kGuard:
      case Opcode::kGuardIs: {
        const auto& instr = static_cast<const DeoptBase&>(i);
        auto kind = InstrGuardKind::kNotZero;
        if (instr.IsCheckNeg()) {
          kind = InstrGuardKind::kNotNegative;
        } else if (instr.IsGuardIs()) {
          kind = InstrGuardKind::kIs;
        }
        appendGuard(bbb, kind, instr, bbb.getDefInstr(instr.GetOperand(0)));
        break;
      }
      case Opcode::kGuardType: {
        const auto& instr = static_cast<const DeoptBase&>(i);
        Instruction* value = bbb.getDefInstr(instr.GetOperand(0));
        appendGuard(bbb, InstrGuardKind::kHasType, instr, value);
        break;
      }
      case Opcode::kRefineType: {
        break;
      }
      case Opcode::kLoadGlobalCached: {
        JIT_DCHECK(
            getConfig().stable_frame,
            "Can only use LoadGlobalCached when frame data is stable across "
            "function calls");
        ThreadedCompileSerialize guard;
        auto instr = static_cast<const LoadGlobalCached*>(&i);
        PyObject* globals = instr->globals();
        JIT_CHECK(
            PyDict_CheckExact(globals),
            "Globals should be a dict, but is actually a {}",
            Py_TYPE(globals)->tp_name);
        env_->code_rt->addReference(globals);
        PyObject* builtins = instr->builtins();
        JIT_CHECK(
            PyDict_CheckExact(builtins),
            "Builtins should be a dict, but is actually a {}",
            Py_TYPE(builtins)->tp_name);
        env_->code_rt->addReference(builtins);
        PyObject* name =
            PyTuple_GET_ITEM(instr->code()->co_names, instr->name_idx());
        JIT_CHECK(
            PyUnicode_CheckExact(name),
            "Global name should be a string, but is actually a {}",
            Py_TYPE(name)->tp_name);
        auto cache = cinderx::getModuleState()->cache_manager->getGlobalCache(
            builtins, globals, name);
        bbb.appendInstr(instr->output(), Instruction::kMove, MemImm{cache});
        break;
      }
      case Opcode::kLoadGlobal: {
        auto instr = static_cast<const LoadGlobal*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        if (!getConfig().stable_frame) {
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_LoadGlobalFromThreadState,
              env_->asm_tstate,
              name);
          break;
        }
        PyObject* builtins = instr->frameState()->builtins;
        env_->code_rt->addReference(builtins);
        PyObject* globals = instr->frameState()->globals;
        env_->code_rt->addReference(globals);
        bbb.appendCallInstruction(
            instr->output(), JITRT_LoadGlobal, globals, builtins, name);
        break;
      }
      case Opcode::kStoreAttr: {
        auto instr = static_cast<const StoreAttrCached*>(&i);
        hir::Register* base = instr->GetOperand(0);
        Instruction* name = getNameFromIdx(bbb, instr);
        hir::Register* value = instr->GetOperand(1);
        Instruction* result = bbb.appendCallInstruction(
            OutVReg{OperandBase::k32bit}, PyObject_SetAttr, base, name, value);
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, result);
        break;
      }
      case Opcode::kStoreAttrCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use StoreAttrCached");
        auto instr = static_cast<const StoreAttrCached*>(&i);
        hir::Register* base = instr->GetOperand(0);
        Instruction* name = getNameFromIdx(bbb, instr);
        hir::Register* value = instr->GetOperand(1);
        auto cache = getContext()->allocateStoreAttrCache();
        Instruction* result = bbb.appendCallInstruction(
            OutVReg{OperandBase::k32bit},
            jit::StoreAttrCache::invoke,
            cache,
            base,
            name,
            value);
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, result);
        break;
      }
      case Opcode::kVectorCall: {
        auto& hir_instr = static_cast<const VectorCall&>(i);
        if (TranslateSpecializedCall(bbb, hir_instr)) {
          break;
        }
        size_t flags = 0;
        uint64_t func = reinterpret_cast<uint64_t>(_PyObject_VectorcallTstate);
        if (!(hir_instr.func()->type() <= TFunc)) {
          // Calls to things which aren't simple Python functions will
          // need to check the eval breaker. We do this in a helper instead
          // of injecting it after every call.
          func = reinterpret_cast<uint64_t>(JITRT_VectorcallTstate);
        }
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kVectorCallTstate,
            // TASK(T140174965): This should be MemImm.
            Imm{func},
            Imm{flags},
            VReg{env_->asm_tstate});
        for (hir::Register* arg : hir_instr.GetOperands()) {
          instr->addOperands(VReg{bbb.getDefInstr(arg)});
        }
        if (!(hir_instr.flags() & CallFlags::KwArgs)) {
          // TASK(T140174965): This should be MemImm.
          instr->addOperands(Imm{0});
        }
        break;
      }
      case Opcode::kCallCFunc: {
        const auto kFuncPtrMap = std::to_array({
#define FUNC_PTR(name, ...) (void*)name,
            CallCFunc_FUNCS(FUNC_PTR)
#undef FUNC_PTR
        });

        auto& hir_instr = static_cast<const CallCFunc&>(i);
        auto func_ptr = kFuncPtrMap[static_cast<int>(hir_instr.func())];
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kCall,
            Imm{reinterpret_cast<uint64_t>(func_ptr)});
        for (hir::Register* arg : hir_instr.GetOperands()) {
          instr->addOperands(VReg{bbb.getDefInstr(arg)});
        }
        break;
      }
      case Opcode::kCallEx: {
        auto& instr = static_cast<const CallEx&>(i);
        bbb.appendCallInstruction(
            instr.output(),
            JITRT_CallFunctionEx,
            instr.func(),
            instr.pargs(),
            instr.kwargs());
        break;
      }
      case Opcode::kCallInd: {
        auto& hir_instr = static_cast<const CallInd&>(i);
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kCall,
            VReg{bbb.getDefInstr(hir_instr.func())});
        for (std::size_t op = 0; op < hir_instr.arg_count(); op++) {
          instr->addOperands(VReg{bbb.getDefInstr(hir_instr.arg(op))});
        }
        auto kind = InstrGuardKind::kNotZero;
        Type ret_type = hir_instr.ret_type();
        if (ret_type <= TCDouble) {
          appendGuard(
              bbb,
              kind,
              hir_instr,
              PhyReg{
                  codegen::arch::reg_double_auxilary_return_loc,
                  DataType::kDouble});
        } else if (ret_type <= TPrimitive) {
          appendGuard(
              bbb,
              kind,
              hir_instr,
              PhyReg{
                  codegen::arch::reg_general_auxilary_return_loc,
                  DataType::k32bit});
        } else {
          appendGuard(bbb, kind, hir_instr, hir_instr.output());
        }
        break;
      }
      case Opcode::kCallIntrinsic: {
        auto& hir_instr = static_cast<const CallIntrinsic&>(i);
        uint64_t func_addr;
        switch (hir_instr.NumOperands()) {
          case 1: {
#if PY_VERSION_HEX >= 0x030E0000
            const intrinsic_func1 func =
                _PyIntrinsics_UnaryFunctions[hir_instr.index()].func;
#else
            const instrinsic_func1 func =
                _PyIntrinsics_UnaryFunctions[hir_instr.index()];
#endif
            func_addr = reinterpret_cast<uint64_t>(func);
            break;
          }
          case 2: {
#if PY_VERSION_HEX >= 0x030E0000
            const intrinsic_func2 func =
                _PyIntrinsics_BinaryFunctions[hir_instr.index()].func;
#else
            const instrinsic_func2 func =
                _PyIntrinsics_BinaryFunctions[hir_instr.index()];
#endif
            func_addr = reinterpret_cast<uint64_t>(func);
            break;
          }
          default:
            JIT_ABORT(
                "CallIntrinsic only supported with 1 or 2 args, got {}",
                hir_instr.NumOperands());
        }
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(), Instruction::kCall, Imm{func_addr});
        instr->addOperands(VReg{env_->asm_tstate});
        for (hir::Register* arg : hir_instr.GetOperands()) {
          instr->addOperands(VReg{bbb.getDefInstr(arg)});
        }
        break;
      }
      case Opcode::kCallMethod: {
        auto& hir_instr = static_cast<const CallMethod&>(i);
        size_t flags = 0;
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kVectorCallTstate,
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(JITRT_Call)},
            Imm{flags},
            VReg{env_->asm_tstate});
        for (hir::Register* arg : hir_instr.GetOperands()) {
          instr->addOperands(VReg{bbb.getDefInstr(arg)});
        }
        if (!(hir_instr.flags() & CallFlags::KwArgs)) {
          // TASK(T140174965): This should be MemImm.
          instr->addOperands(Imm{0});
        }
        break;
      }

      case Opcode::kCallStatic: {
        auto& hir_instr = static_cast<const CallStatic&>(i);
        std::vector<Instruction*> args;
        // Generate the argument conversions before the call.
        for (hir::Register* reg_arg : hir_instr.GetOperands()) {
          Instruction* arg = bbb.getDefInstr(reg_arg);
          Type src_type = reg_arg->type();
          if (src_type <= (TCBool | TCUInt8 | TCUInt16)) {
            arg = bbb.appendInstr(
                Instruction::kZext, OutVReg{OperandBase::k64bit}, arg);
          } else if (src_type <= (TCInt8 | TCInt16)) {
            arg = bbb.appendInstr(
                Instruction::kSext, OutVReg{OperandBase::k64bit}, arg);
          }
          args.push_back(arg);
        }
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kCall,
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(hir_instr.addr())});
        for (const auto& arg : args) {
          instr->addOperands(VReg{arg});
        }
        break;
      }
      case Opcode::kCallStaticRetVoid: {
        auto& hir_instr = static_cast<const CallStaticRetVoid&>(i);
        Instruction* instr = bbb.appendInstr(
            Instruction::kCall,
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(hir_instr.addr())});
        for (hir::Register* arg : hir_instr.GetOperands()) {
          instr->addOperands(VReg{bbb.getDefInstr(arg)});
        }
        break;
      }
      case Opcode::kInvokeStaticFunction: {
        ThreadedCompileSerialize guard;

        auto instr = static_cast<const InvokeStaticFunction*>(&i);
        auto nargs = instr->NumOperands();
        PyFunctionObject* func = instr->func();

        std::stringstream ss;
        Instruction* lir;
        if (isJitCompiled(func)) {
          lir = bbb.appendInstr(
              instr->output(),
              Instruction::kCall,
              Imm{reinterpret_cast<uint64_t>(
                  JITRT_GET_STATIC_ENTRY(func->vectorcall))});
        } else {
          void** indir = env_->ctx->findFunctionEntryCache(func);
          env_->function_indirections.emplace(func, indir);
          Instruction* move = bbb.appendInstr(
              OutVReg{OperandBase::k64bit}, Instruction::kMove, MemImm{indir});

          lir = bbb.appendInstr(instr->output(), Instruction::kCall, move);
        }

        for (size_t argIdx = 0; argIdx < nargs; argIdx++) {
          lir->addOperands(VReg{bbb.getDefInstr(instr->GetOperand(argIdx))});
        }
        // functions that return primitives will signal error via edx/xmm1
        auto kind = InstrGuardKind::kNotZero;
        Type ret_type = instr->ret_type();
        if (ret_type <= TCDouble) {
          appendGuard(
              bbb,
              kind,
              *instr,
              PhyReg{
                  codegen::arch::reg_double_auxilary_return_loc,
                  OperandBase::kDouble});
        } else if (ret_type <= TPrimitive) {
          appendGuard(
              bbb,
              kind,
              *instr,
              PhyReg{
                  codegen::arch::reg_general_auxilary_return_loc,
                  OperandBase::k32bit});
        } else {
          appendGuard(bbb, kind, *instr, instr->output());
        }
        break;
      }

      case Opcode::kLoadField: {
        auto instr = static_cast<const LoadField*>(&i);
        hir::Register* dest = instr->output();
        Instruction* receiver = bbb.getDefInstr(instr->receiver());
        auto offset = static_cast<int32_t>(instr->offset());
        bbb.appendInstr(dest, Instruction::kMove, Ind{receiver, offset});
        break;
      }

      case Opcode::kLoadFieldAddress: {
        auto instr = static_cast<const LoadFieldAddress*>(&i);
        hir::Register* dest = instr->output();
        Instruction* object = bbb.getDefInstr(instr->object());
        Instruction* offset = bbb.getDefInstr(instr->offset());
        bbb.appendInstr(dest, Instruction::kLea, Ind{object, offset});
        break;
      }

      case Opcode::kStoreField: {
        auto instr = static_cast<const StoreField*>(&i);
        Instruction* lir = bbb.appendInstr(
            OutInd{
                bbb.getDefInstr(instr->receiver()),
                static_cast<int32_t>(instr->offset())},
            Instruction::kMove,
            instr->value());
        lir->output()->setDataType(lir->getInput(0)->dataType());
        break;
      }

      case Opcode::kCast: {
        auto instr = static_cast<const Cast*>(&i);
        PyObject* (*func)(PyObject*, PyTypeObject*);
        if (instr->pytype() == &PyFloat_Type) {
          bbb.appendCallInstruction(
              instr->output(),
              instr->optional() ? JITRT_CastToFloatOptional : JITRT_CastToFloat,
              instr->value());
          break;
        } else if (instr->exact()) {
          func = instr->optional() ? JITRT_CastOptionalExact : JITRT_CastExact;
        } else {
          func = instr->optional() ? JITRT_CastOptional : JITRT_Cast;
        }

        bbb.appendCallInstruction(
            instr->output(), func, instr->value(), instr->pytype());
        break;
      }

      case Opcode::kTpAlloc: {
        auto instr = static_cast<const TpAlloc*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            instr->pytype()->tp_alloc,
            instr->pytype(),
            /*nitems=*/static_cast<Py_ssize_t>(0));
        break;
      }

      case Opcode::kMakeList: {
        auto instr = static_cast<const MakeList*>(&i);
        Instruction* call = bbb.appendCallInstruction(
            instr->output(),
            PyList_New,
            static_cast<Py_ssize_t>(instr->nvalues()));
        if (instr->nvalues() > 0) {
          // TODO(T174544781): need to check for nullptr before initializing,
          // currently that check only happens after assigning these values.
          Instruction* load = bbb.appendInstr(
              Instruction::kMove,
              OutVReg{OperandBase::k64bit},
              Ind{call, offsetof(PyListObject, ob_item)});
          for (size_t valueIdx = 0; valueIdx < instr->nvalues(); valueIdx++) {
            bbb.appendInstr(
                OutInd{load, static_cast<int32_t>(valueIdx * kPointerSize)},
                Instruction::kMove,
                instr->GetOperand(valueIdx));
          }
        }
        break;
      }
      case Opcode::kMakeTuple: {
        auto instr = static_cast<const MakeTuple*>(&i);
        Instruction* tuple =
            bbb.appendInstr(instr->output(), Instruction::kVarArgCall);
        tuple->addOperands(
            Imm{reinterpret_cast<uint64_t>(
#if PY_VERSION_HEX >= 0x030F0000
                PyTuple_FromArray
#else
                _PyTuple_FromArray
#endif
                )});
        for (size_t ix = 0; ix < instr->NumOperands(); ix++) {
          tuple->addOperands(VReg{bbb.getDefInstr(instr->GetOperand(ix))});
        }
        break;
      }
      case Opcode::kMatchClass: {
        const auto& instr = static_cast<const MatchClass&>(i);
        bbb.appendCallInstruction(
            instr.output(),
            _PyEval_MatchClass,
            env_->asm_tstate,
            instr.GetOperand(0),
            instr.GetOperand(1),
            instr.GetOperand(2),
            instr.GetOperand(3));
        break;
      }
      case Opcode::kMatchKeys: {
        auto instr = static_cast<const MatchKeys*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            _PyEval_MatchKeys,
            env_->asm_tstate,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kLoadTupleItem: {
        auto instr = static_cast<const LoadTupleItem*>(&i);
        hir::Register* dest = instr->output();
        Instruction* tuple = bbb.getDefInstr(instr->tuple());
        auto item_offset = static_cast<int32_t>(
            offsetof(PyTupleObject, ob_item) + instr->idx() * kPointerSize);
        bbb.appendInstr(dest, Instruction::kMove, Ind{tuple, item_offset});
        break;
      }
      case Opcode::kCheckSequenceBounds: {
        auto instr = static_cast<const CheckSequenceBounds*>(&i);
        auto type = instr->GetOperand(1)->type();
        if (type <= (TCInt8 | TCInt16 | TCInt32) ||
            type <= (TCUInt8 | TCUInt16 | TCUInt32)) {
          Instruction* lir = bbb.appendInstr(
              Instruction::kSext, OutVReg{}, instr->GetOperand(1));
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_CheckSequenceBounds,
              instr->GetOperand(0),
              lir);
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_CheckSequenceBounds,
              instr->GetOperand(0),
              instr->GetOperand(1));
        }
        break;
      }
      case Opcode::kLoadArrayItem: {
        auto instr = static_cast<const LoadArrayItem*>(&i);
        hir::Register* dest = instr->output();
        Instruction* ob_item = bbb.getDefInstr(instr->ob_item());
        Instruction* idx = bbb.getDefInstr(instr->idx());
        int32_t offset = instr->offset();
        // Might know the index at compile-time.
        auto ind = Ind{ob_item, idx, instr->type().sizeInBytes(), offset};
        if (instr->idx()->type().hasIntSpec()) {
          auto scaled_offset = static_cast<int32_t>(
              instr->idx()->type().intSpec() * instr->type().sizeInBytes() +
              offset);
          ind = Ind{ob_item, scaled_offset};
        }
        bbb.appendInstr(dest, Instruction::kMove, ind);
        break;
      }
      case Opcode::kStoreArrayItem: {
        auto instr = static_cast<const StoreArrayItem*>(&i);
        Instruction* ob_item = bbb.getDefInstr(instr->ob_item());
        Instruction* idx = bbb.getDefInstr(instr->idx());
        Instruction* value = bbb.getDefInstr(instr->value());
        auto sizeBytes = instr->type().sizeInBytes();
        auto dt = hirTypeToDataType(instr->type());
        // Might know the index at compile-time.
        auto ind = OutInd{ob_item, idx, sizeBytes, 0, dt};
        if (instr->idx()->type().hasIntSpec()) {
          auto scaled_offset =
              static_cast<int32_t>(instr->idx()->type().intSpec() * sizeBytes);
          ind = OutInd{ob_item, scaled_offset, dt};
        }
        bbb.appendInstr(ind, Instruction::kMove, value);
        break;
      }
      case Opcode::kLoadSplitDictItem: {
        auto instr = static_cast<const LoadSplitDictItem*>(&i);
        Register* dict = instr->GetOperand(0);
        // Users of LoadSplitDictItem are required to verify that dict has a
        // split table, so it's safe to load and access ma_values with no
        // additional checks here.
        Instruction* ma_values = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{bbb.getDefInstr(dict),
                static_cast<int32_t>(offsetof(PyDictObject, ma_values))});
        bbb.appendInstr(
            instr->output(),
            Instruction::kMove,
            Ind{ma_values,
                static_cast<int32_t>(instr->itemIdx() * sizeof(PyObject*))});
        break;
      }
      case Opcode::kMakeCheckedList: {
        auto instr = static_cast<const MakeCheckedList*>(&i);
        auto capacity = instr->nvalues();
        bbb.appendCallInstruction(
            instr->output(),
            Ci_CheckedList_New,
            instr->type().typeSpec(),
            static_cast<Py_ssize_t>(capacity));
        if (instr->nvalues() > 0) {
          Instruction* ob_item = bbb.appendInstr(
              OutVReg{},
              Instruction::kMove,
              Ind{bbb.getDefInstr(instr->output()),
                  static_cast<int32_t>(offsetof(PyListObject, ob_item))});
          for (size_t valueIdx = 0; valueIdx < instr->nvalues(); valueIdx++) {
            bbb.appendInstr(
                OutInd{ob_item, static_cast<int32_t>(valueIdx * kPointerSize)},
                Instruction::kMove,
                instr->GetOperand(valueIdx));
          }
        }
        break;
      }
      case Opcode::kMakeCheckedDict: {
        auto instr = static_cast<const MakeCheckedDict*>(&i);
        auto capacity = instr->GetCapacity();
        if (capacity == 0) {
          bbb.appendCallInstruction(
              instr->output(), Ci_CheckedDict_New, instr->type().typeSpec());
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              Ci_CheckedDict_NewPresized,
              instr->type().typeSpec(),
              static_cast<Py_ssize_t>(capacity));
        }
        break;
      }
      case Opcode::kMakeDict: {
        auto instr = static_cast<const MakeDict*>(&i);
        auto capacity = instr->GetCapacity();
        if (capacity == 0) {
          bbb.appendCallInstruction(instr->output(), PyDict_New);
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              _PyDict_NewPresized,
              static_cast<Py_ssize_t>(capacity));
        }
        break;
      }
      case Opcode::kMakeSet: {
        auto instr = static_cast<const MakeSet*>(&i);
        bbb.appendCallInstruction(instr->output(), PySet_New, nullptr);
        break;
      }
      case Opcode::kDictUpdate: {
        bbb.appendCallInstruction(
            i.output(),
            JITRT_DictUpdate,
            env_->asm_tstate,
            i.GetOperand(0),
            i.GetOperand(1));
        break;
      }
      case Opcode::kDictMerge: {
        bbb.appendCallInstruction(
            i.output(),
            JITRT_DictMerge,
            env_->asm_tstate,
            i.GetOperand(0),
            i.GetOperand(1),
            i.GetOperand(2));
        break;
      }
      case Opcode::kMergeSetUnpack: {
        auto instr = static_cast<const MergeSetUnpack*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            _PySet_Update,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kSetDictItem: {
        auto instr = static_cast<const SetDictItem*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
#ifdef Py_GIL_DISABLED
            // TODO(T250369690): Need thread-safe checked collections
            PyDict_SetItem,
#else
            Ci_DictOrChecked_SetItem,
#endif
            instr->GetOperand(0),
            instr->GetOperand(1),
            instr->GetOperand(2));
        break;
      }
      case Opcode::kSetSetItem: {
        auto instr = static_cast<const SetSetItem*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            PySet_Add,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kSetUpdate: {
        auto instr = static_cast<const SetUpdate*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            _PySet_Update,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kStoreSubscr: {
        auto instr = static_cast<const StoreSubscr*>(&i);
        Instruction* result = bbb.appendCallInstruction(
            OutVReg{OperandBase::k32bit},
            PyObject_SetItem,
            instr->GetOperand(0),
            instr->GetOperand(1),
            instr->GetOperand(2));
        appendGuard(bbb, InstrGuardKind::kNotNegative, *instr, result);
        break;
      }
      case Opcode::kDictSubscr: {
        auto instr = static_cast<const DictSubscr*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            PyDict_Type.tp_as_mapping->mp_subscript,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kInPlaceOp: {
        auto instr = static_cast<const InPlaceOp*>(&i);

        // NB: This needs to be in the order that the values appear in the
        // InPlaceOpKind enum
        static const binaryfunc helpers[] = {
            PyNumber_InPlaceAdd,
            PyNumber_InPlaceAnd,
            PyNumber_InPlaceFloorDivide,
            PyNumber_InPlaceLshift,
            PyNumber_InPlaceMatrixMultiply,
            PyNumber_InPlaceRemainder,
            PyNumber_InPlaceMultiply,
            PyNumber_InPlaceOr,
            nullptr, // Power is a ternaryfunc
            PyNumber_InPlaceRshift,
            PyNumber_InPlaceSubtract,
            PyNumber_InPlaceTrueDivide,
            PyNumber_InPlaceXor,
        };
        JIT_CHECK(
            static_cast<unsigned long>(instr->op()) < sizeof(helpers),
            "unsupported inplaceop");

        auto op_kind = static_cast<int>(instr->op());

        if (instr->op() != InPlaceOpKind::kPower) {
          bbb.appendCallInstruction(
              instr->output(), helpers[op_kind], instr->left(), instr->right());
        } else {
          bbb.appendCallInstruction(
              instr->output(),
              PyNumber_InPlacePower,
              instr->left(),
              instr->right(),
              Py_None);
        }
        break;
      }
      case Opcode::kBranch: {
        break;
      }
      case Opcode::kBuildSlice: {
        auto instr = static_cast<const BuildSlice*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
            PySlice_New,
            instr->start(),
            instr->stop(),
            instr->step() != nullptr ? instr->step() : nullptr);

        break;
      }
      case Opcode::kGetIter: {
        auto instr = static_cast<const GetIter*>(&i);
        bbb.appendCallInstruction(
            instr->output(), PyObject_GetIter, instr->GetOperand(0));
        break;
      }
      case Opcode::kGetLength: {
        auto instr = static_cast<const GetLength*>(&i);
        bbb.appendCallInstruction(
            instr->output(), JITRT_GetLength, instr->GetOperand(0));
        break;
      }
      case Opcode::kPhi: {
        auto instr = static_cast<const Phi*>(&i);
        bbb.appendInstr(instr->output(), Instruction::kPhi);
        // The phi's operands will get filled out later, once we have LIR
        // definitions for all HIR values.
        break;
      }
      case Opcode::kMakeFunction: {
        auto instr = static_cast<const MakeFunction*>(&i);
        auto code = instr->GetOperand(0);
        auto qualname = instr->GetOperand(1);

        Instruction* globals;
        if (getConfig().stable_frame) {
          BorrowedRef<> obj = instr->frameState()->globals;
          env_->code_rt->addReference(obj);
          globals = bbb.appendInstr(
              OutVReg{},
              Instruction::kMove,
              // TASK(T140174965): This should be MemImm.
              Imm{reinterpret_cast<uint64_t>(obj.get()), OperandBase::kObject});
        } else {
          globals = bbb.appendInstr(
              OutVReg{},
              Instruction::kCall,
              // TASK(T140174965): This should be MemImm.
              Imm{reinterpret_cast<uint64_t>(JITRT_LoadGlobalsDict)},
              env_->asm_tstate);
        }

        if (!qualname->isA(TNullptr)) {
          bbb.appendCallInstruction(
              instr->output(),
              PyFunction_NewWithQualName,
              code,
              globals,
              qualname);
        } else {
          bbb.appendCallInstruction(
              instr->output(), PyFunction_New, code, globals);
        }
        break;
      }
      case Opcode::kSetFunctionAttr: {
        auto instr = static_cast<const SetFunctionAttr*>(&i);

        bbb.appendInstr(
            OutInd{
                bbb.getDefInstr(instr->base()),
                static_cast<int32_t>(instr->offset())},
            Instruction::kMove,
            instr->value());
        break;
      }
      case Opcode::kListAppend: {
        auto instr = static_cast<const ListAppend*>(&i);

        bbb.appendCallInstruction(
            instr->output(),
#ifdef Py_GIL_DISABLED
            // TODO(T250369690): Need thread-safe checked collections
            PyList_Append,
#else
            Ci_ListOrCheckedList_Append,
#endif
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kListExtend: {
        auto instr = static_cast<const ListExtend*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            __Invoke_PyList_Extend,
            env_->asm_tstate,
            instr->GetOperand(0),
            instr->GetOperand(1));
        break;
      }
      case Opcode::kMakeTupleFromList: {
        auto instr = static_cast<const MakeTupleFromList*>(&i);
        bbb.appendCallInstruction(
            instr->output(), PyList_AsTuple, instr->GetOperand(0));
        break;
      }
      case Opcode::kGetTuple: {
        auto instr = static_cast<const GetTuple*>(&i);

        bbb.appendCallInstruction(
            instr->output(), PySequence_Tuple, instr->GetOperand(0));
        break;
      }
      case Opcode::kInvokeIterNext: {
        auto instr = static_cast<const InvokeIterNext*>(&i);
        bbb.appendCallInstruction(
            instr->output(), JITRT_InvokeIterNext, instr->GetOperand(0));
        break;
      }
      case Opcode::kLoadEvalBreaker: {
        hir::Register* dest = i.output();
#if PY_VERSION_HEX >= 0x030D0000
        Instruction* tstate = env_->asm_tstate;
        // tstate->ceval.eval_breaker
        static_assert(
            sizeof(reinterpret_cast<PyThreadState*>(0)->eval_breaker) == 8,
            "Eval breaker is not a 8 byte value");
        bbb.appendInstr(
            dest,
            Instruction::kMoveRelaxed,
            Ind{tstate, offsetof(PyThreadState, eval_breaker)});
#else
        // eval_breaker is in the runtime, which the code is generated against,
        // load it directly.
        static_assert(
            sizeof(reinterpret_cast<PyThreadState*>(0)
                       ->interp->ceval.eval_breaker) == 4,
            "Eval breaker is not a 4 byte value");
        bbb.appendInstr(
            dest,
            Instruction::kMoveRelaxed,
            MemImm{reinterpret_cast<int*>(
                &ThreadedCompileContext::interpreter()->ceval.eval_breaker)});
#endif
        break;
      }
      case Opcode::kAtQuiescentState: {
#ifdef Py_GIL_DISABLED
        bbb.appendInvokeInstruction(JITRT_AtQuiescentState, env_->asm_tstate);
#endif
        break;
      }
      case Opcode::kRunPeriodicTasks: {
        auto helper = _Py_HandlePending;
        bbb.appendCallInstruction(i.output(), helper, env_->asm_tstate);
        break;
      }
      case Opcode::kSnapshot: {
        // Snapshots are purely informative
        break;
      }
      case Opcode::kUseType: {
        // UseTypes are purely informative
        break;
      }
      case Opcode::kHintType: {
        // HintTypes are purely informative
        break;
      }
      case Opcode::kBeginInlinedFunction: {
        JIT_DCHECK(
            getConfig().stable_frame,
            "Inlined code stores references to code objects");
#if defined(ENABLE_LIGHTWEIGHT_FRAMES)
        auto instr = static_cast<const BeginInlinedFunction*>(&i);
        // Set code object data
        BorrowedRef<PyCodeObject> code = instr->code();
        env_->code_rt->addReference(code.getObj());
        PyObject* globals = instr->globals();
        env_->code_rt->addReference(globals);
        PyObject* builtins = instr->builtins();
        env_->code_rt->addReference(builtins);
        PyObject* func = instr->func();
        env_->code_rt->addReference(func);
        RuntimeFrameState* rtfs = env_->code_rt->allocateRuntimeFrameState(
            code, builtins, globals, func);
#endif
#if defined(ENABLE_LIGHTWEIGHT_FRAMES)
        // Load the address of our _PyInterpreterFrame and the previous
        // _PyInterpreterFrame we skip past the FrameHeader for this.
        Instruction* caller_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kLea,
            Stk{PhyLocation(
                static_cast<int32_t>(
                    frameOffsetBefore(instr) + sizeof(FrameHeader)))});

        // There is already an interpreter frame for the caller function.
        Instruction* callee_frame = getInlinedFrame(bbb, instr);
        // Store code
#if PY_VERSION_HEX >= 0x030E0000
        // Store frame helper as f_executable
        BorrowedRef<> frame_reifier;
        auto existing_reifier = inline_code_to_reifier_.find(code);
        if (existing_reifier == inline_code_to_reifier_.end()) {
          frame_reifier = instr->reifier();
          env_->code_rt->addReference(frame_reifier);
          inline_code_to_reifier_.emplace(code, frame_reifier.get());
        } else {
          frame_reifier = existing_reifier->second;
        }
        Instruction* code_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, frame_reifier.get());
#else
        Instruction* code_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, code.get());
#endif
        bbb.appendInstr(
            OutInd{callee_frame, FRAME_EXECUTABLE_OFFSET},
            Instruction::kMove,
            code_reg);

#if PY_VERSION_HEX >= 0x030E0000
        // Store function
        PyObject* func_val = func;
#else
        // Store frame helper as f_funcobj
        PyObject* func_val = cinderx::getModuleState()->frame_reifier;
#endif
        Instruction* func_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, func_val);
        bbb.appendInstr(
            OutInd{callee_frame, offsetof(_PyInterpreterFrame, f_funcobj)},
            Instruction::kMove,
            func_reg);

        // Store RTFS in FrameHeader as a tag
        Instruction* rtfs_reg = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            reinterpret_cast<uintptr_t>(rtfs) | JIT_FRAME_RTFS);
        bbb.appendInstr(
            OutInd{
                callee_frame,
                (Py_ssize_t)offsetof(FrameHeader, func) -
                    (Py_ssize_t)sizeof(FrameHeader)},
            Instruction::kMove,
            rtfs_reg);

        bbb.appendInstr(
            OutInd{callee_frame, offsetof(_PyInterpreterFrame, previous)},
            Instruction::kMove,
            caller_frame);

#if PY_VERSION_HEX >= 0x030E0000
        Instruction* localsplus = bbb.appendInstr(
            OutVReg{},
            Instruction::kLea,
            Stk{PhyLocation(
                static_cast<int32_t>(
                    frameOffsetOf(instr) +
                    offsetof(_PyInterpreterFrame, localsplus)))});

        bbb.appendInstr(
            OutInd{callee_frame, offsetof(_PyInterpreterFrame, stackpointer)},
            Instruction::kMove,
            localsplus);

        bbb.appendInstr(
            OutInd{callee_frame, offsetof(_PyInterpreterFrame, f_locals)},
            Instruction::kMove,
            Imm{0});

        // Store prev_instr
        _Py_CODEUNIT* frame_code = _PyCode_CODE(code.get());
#else
        // Store prev_instr
        _Py_CODEUNIT* frame_code = _PyCode_CODE(code.get()) - 1;
#endif

        Instruction* codeunit_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, frame_code);

        bbb.appendInstr(
            OutInd{callee_frame, FRAME_INSTR_OFFSET},
            Instruction::kMove,
            codeunit_reg);

#ifdef Py_GIL_DISABLED
        bbb.appendInstr(
            OutInd{
                callee_frame,
                offsetof(_PyInterpreterFrame, tlbc_index),
                OperandBase::k32bit},
            Instruction::kMove,
            Imm{0, OperandBase::k32bit});
#endif

        bbb.appendInstr(
            OutInd{
                callee_frame,
                offsetof(_PyInterpreterFrame, owner),
                OperandBase::k8bit},
            Instruction::kMove,
            Imm{static_cast<uint8_t>(FRAME_OWNED_BY_THREAD),
                OperandBase::k8bit});
#if PY_VERSION_HEX < 0x030E0000
        if (!_Py_IsImmortal(code.get()))
#endif
        {
          makeIncref(bbb, code_reg, false);
        }

        // Set our frame as top of stack
#if PY_VERSION_HEX >= 0x030D0000
        if (!_Py_IsImmortal(func_val)) {
          makeIncref(bbb, func_reg, false);
        }
        bbb.appendInstr(
            OutInd{env_->asm_tstate, offsetof(PyThreadState, current_frame)},
            Instruction::kMove,
            callee_frame);
#else
        Instruction* cframe_reg = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{env_->asm_tstate, offsetof(PyThreadState, cframe)});
        bbb.appendInstr(
            OutInd{cframe_reg, offsetof(_PyCFrame, current_frame)},
            Instruction::kMove,
            callee_frame);
#endif

#endif
        break;
      }
      case Opcode::kEndInlinedFunction: {
#if defined(ENABLE_LIGHTWEIGHT_FRAMES)
        JIT_CHECK(
            getConfig().frame_mode == FrameMode::kLightweight,
            "Can only generate LIR for inlined functions in 3.12+ when "
            "lightweight frames are enabled");

        auto instr = static_cast<const EndInlinedFunction&>(i);

        // Test to see if RTFS is still in place
        Instruction* callee_frame = getInlinedFrame(bbb, instr.matchingBegin());
        auto rtfs_reg = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{callee_frame,
                (Py_ssize_t)offsetof(FrameHeader, func) -
                    (Py_ssize_t)sizeof(FrameHeader)});

        JIT_DCHECK(
            JIT_FRAME_INITIALIZED == 2,
            "JIT_FRAME_INITIALIZED changed"); // this is the bit we're testing
                                              // below
        bbb.appendInstr(Instruction::kBitTest, rtfs_reg, Imm{1});
        auto done_block = bbb.allocateBlock();
        auto not_materialized_block = bbb.allocateBlock();
        // kBitTest lowers differently per architecture:
        // - x86: BT sets the Carry flag to the tested bit value, so
        //   kBranchNC (jnc) branches when the bit is NOT set.
        // - ARM64: BT is lowered to TST which sets the Zero flag, so
        //   kBranchE (b.eq) branches when the bit is NOT set.
        bbb.appendBranch(
            codegen::arch::kBuildArch == codegen::arch::Arch::kAarch64
                ? Instruction::kBranchE
                : Instruction::kBranchNC,
            not_materialized_block);
        bbb.appendBlock(bbb.allocateBlock());

        // The frame was materialized, let's use the unlink helper to clean
        // things up.
        bbb.appendInvokeInstruction(JITRT_UnlinkFrame, env_->asm_tstate);
        bbb.appendBranch(Instruction::kBranch, done_block);

        // The frame was not materialized, we just need to update thread state
        // to point at the caller and maybe decref the code object.
        bbb.switchBlock(not_materialized_block);
        // The frame was never materialized, we just need to unlink the frame
        // and potentiall decref the code object.
        Instruction* caller_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kLea,
            Stk{PhyLocation(
                static_cast<int32_t>(
                    frameOffsetBefore(instr.matchingBegin()) +
                    sizeof(FrameHeader)))});
#if PY_VERSION_HEX >= 0x030D0000
        bbb.appendInstr(
            OutInd{env_->asm_tstate, offsetof(PyThreadState, current_frame)},
            Instruction::kMove,
            caller_frame);
#else
        Instruction* cframe_reg = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{env_->asm_tstate, offsetof(PyThreadState, cframe)});
        bbb.appendInstr(
            OutInd{cframe_reg, offsetof(_PyCFrame, current_frame)},
            Instruction::kMove,
            caller_frame);
#endif
        auto code = instr.matchingBegin()->code();
#if PY_VERSION_HEX >= 0x030E0000
        auto reifier = inline_code_to_reifier_.at(code.get());
        Instruction* reifier_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, reifier.get());
        makeDecref(
            bbb,
            reifier_reg,
            std::optional<destructor>(
                PyUnstable_JITExecutable_Type.tp_dealloc));
#if PY_VERSION_HEX < 0x030F0000
        // On 3.14, we stored the function object in f_funcobj and incref'd it.
        // Need to decref it here since the frame was not materialized.
        PyObject* func = instr.matchingBegin()->func();
        if (!_Py_IsImmortal(func)) {
          Instruction* func_reg =
              bbb.appendInstr(OutVReg{}, Instruction::kMove, func);
          makeDecref(
              bbb,
              func_reg,
              std::optional<destructor>(PyFunction_Type.tp_dealloc));
        }
#endif
#else
        if (!_Py_IsImmortal(code.get())) {
          Instruction* code_reg =
              bbb.appendInstr(OutVReg{}, Instruction::kMove, code.get());
          makeDecref(
              bbb, code_reg, std::optional<destructor>(PyCode_Type.tp_dealloc));
        }
#endif

        bbb.appendBlock(done_block);
#endif
        break;
      }
      case Opcode::kCompactLongUnbox: {
        // Inline _PyLong_CompactValue: sign * (Py_ssize_t)ob_digit[0]
        // where sign = 1 - (lv_tag & 3).
        Instruction* obj = bbb.getDefInstr(i.GetOperand(0));
        int32_t lv_tag_offset =
            static_cast<int32_t>(offsetof(PyLongObject, long_value.lv_tag));
        int32_t digit_offset =
            static_cast<int32_t>(offsetof(PyLongObject, long_value.ob_digit));
        // Load lv_tag
        Instruction* lv_tag = bbb.appendInstr(
            OutVReg{DataType::k64bit},
            Instruction::kMove,
            Ind{obj, lv_tag_offset});
        // sign = lv_tag & _PyLong_SIGN_MASK (i.e. & 3)
        Instruction* sign_bits = bbb.appendInstr(
            OutVReg{DataType::k64bit},
            Instruction::kAnd,
            lv_tag,
            Imm{_PyLong_SIGN_MASK});
        // sign = 1 - sign_bits
        Instruction* one = bbb.appendInstr(
            OutVReg{DataType::k64bit}, Instruction::kMove, Imm{1});
        Instruction* sign = bbb.appendInstr(
            OutVReg{DataType::k64bit}, Instruction::kSub, one, sign_bits);
        // Load ob_digit[0] as 32-bit unsigned, zero-extend to 64-bit
        Instruction* digit = bbb.appendInstr(
            OutVReg{DataType::k32bit},
            Instruction::kMove,
            Ind{obj, digit_offset});
        Instruction* digit64 = bbb.appendInstr(
            OutVReg{DataType::k64bit}, Instruction::kZext, digit);
        // result = sign * digit
        bbb.appendInstr(i.output(), Instruction::kMul, sign, digit64);
        break;
      }
      case Opcode::kIsCompactLong: {
        Type operand_type = i.GetOperand(0)->type();
        if (operand_type <= TCInt64) {
          // For a raw CInt64, check if the value fits in a single 30-bit
          // digit: -(2^30-1) <= v <= 2^30-1, i.e.
          // (unsigned)(v + (2^30-1)) < (2^31-1).
          Instruction* val = bbb.getDefInstr(i.GetOperand(0));
          constexpr int64_t kMaxDigit = (int64_t{1} << PyLong_SHIFT) - 1;
          Instruction* shifted = bbb.appendInstr(
              OutVReg{DataType::k64bit},
              Instruction::kAdd,
              val,
              Imm{kMaxDigit});
          bbb.appendInstr(
              i.output(),
              Instruction::kLessThanUnsigned,
              shifted,
              Imm{2 * kMaxDigit + 1});
        } else {
          // Load lv_tag from PyLongObject and check < (2 << 3) i.e. < 16.
          Instruction* obj = bbb.getDefInstr(i.GetOperand(0));
          int32_t lv_tag_offset =
              static_cast<int32_t>(offsetof(PyLongObject, long_value.lv_tag));
          Instruction* lv_tag = bbb.appendInstr(
              OutVReg{DataType::k64bit},
              Instruction::kMove,
              Ind{obj, lv_tag_offset});
          bbb.appendInstr(
              i.output(),
              Instruction::kLessThanUnsigned,
              lv_tag,
              Imm{2 << _PyLong_NON_SIZE_BITS});
        }
        break;
      }
      case Opcode::kIsTruthy: {
        auto is_truthy = static_cast<const IsTruthy*>(&i);
        Instruction* call_instr = bbb.appendCallInstruction(
            i.output(), PyObject_IsTrue, i.GetOperand(0));
        appendGuard(bbb, InstrGuardKind::kNotNegative, *is_truthy, call_instr);
        break;
      }
      case Opcode::kImportFrom: {
#if ENABLE_LAZY_IMPORTS
        auto instr = static_cast<const ImportFrom*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(
            i.output(),
            _PyImport_ImportFrom,
            env_->asm_tstate,
            instr->module(),
            name);
#elif PY_VERSION_HEX >= 0x030E0000
        auto instr = static_cast<const ImportFrom*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(
            i.output(),
            _PyEval_ImportFrom,
            env_->asm_tstate,
            instr->module(),
            name);
#else
        JIT_ABORT(
            "IMPORT_FROM is not supported, LIRGenerator has no access to "
            "import_from() from stock CPython");
#endif
        break;
      }
      case Opcode::kImportName: {
        auto instr = static_cast<const ImportName*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        bbb.appendCallInstruction(
            i.output(),
            JITRT_ImportName,
            env_->asm_tstate,
            name,
            instr->GetFromList(),
            instr->GetLevel());
        break;
      }
      case Opcode::kEagerImportName: {
        auto instr = static_cast<const EagerImportName*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
#if PY_VERSION_HEX >= 0x030F0000
        bbb.appendCallInstruction(
            i.output(),
            JITRT_ImportName,
            env_->asm_tstate,
            name,
            instr->GetFromList(),
            instr->GetLevel());
#elif PY_VERSION_HEX >= 0x030E0000
        // asm_interpreter_frame isn't right for inlined functions but we don't
        // allow inlining of things which contain EagerImportName instructions.
        bbb.appendCallInstruction(
            i.output(),
            _PyEval_ImportName,
            env_->asm_tstate,
            env_->asm_interpreter_frame,
            name,
            instr->GetFromList(),
            instr->GetLevel());
#elif ENABLE_LAZY_IMPORTS
        PyObject* globals = instr->frameState()->globals;
        PyObject* builtins = instr->frameState()->builtins;
        PyObject* locals = Py_None; /* see JITRT_ImportName. */
        bbb.appendCallInstruction(
            i.output(),
            _PyImport_ImportName,
            env_->asm_tstate,
            builtins,
            globals,
            locals,
            name,
            instr->GetFromList(),
            instr->GetLevel());
#else
        bbb.appendCallInstruction(i.output(), PyImport_Import, name);
#endif
        break;
      }
      case Opcode::kRaise: {
        const auto& instr = static_cast<const Raise&>(i);
        appendGuardAlwaysFail(bbb, instr);
        break;
      }
      case Opcode::kRaiseStatic: {
        const auto& instr = static_cast<const RaiseStatic&>(i);
        Instruction* lir = bbb.appendInstr(
            Instruction::kCall,
            reinterpret_cast<uint64_t>(PyErr_Format),
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(instr.excType()), DataType::kObject},
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(instr.fmt())});
        for (size_t operandIdx = 0; operandIdx < instr.NumOperands();
             operandIdx++) {
          lir->addOperands(VReg{bbb.getDefInstr(instr.GetOperand(operandIdx))});
        }

        appendGuardAlwaysFail(bbb, instr);
        break;
      }
      case Opcode::kFormatValue: {
        const auto& instr = static_cast<const FormatValue&>(i);
        bbb.appendCallInstruction(
            instr.output(),
            JITRT_FormatValue,
            env_->asm_tstate,
            instr.GetOperand(0),
            instr.GetOperand(1),
            instr.conversion());
        break;
      }
      case Opcode::kFormatWithSpec: {
        const auto& instr = static_cast<const FormatWithSpec&>(i);
        bbb.appendCallInstruction(
            instr.output(),
            PyObject_Format,
            instr.GetOperand(0),
            instr.GetOperand(1));
        break;
      }
      case Opcode::kBuildString: {
        const auto& instr = static_cast<const BuildString&>(i);

        // using vectorcall here although this is not strictly a vector call.
        // tstate and the callable are always null, and all the components
        // to be concatenated will be in the args argument.

        Instruction* lir = bbb.appendInstr(
            instr.output(),
            Instruction::kVectorCallTstate,
            JITRT_BuildString,
            nullptr,
            nullptr);
        lir->addOperands(Imm{0});
        for (size_t operandIdx = 0; operandIdx < instr.NumOperands();
             operandIdx++) {
          lir->addOperands(VReg{bbb.getDefInstr(instr.GetOperand(operandIdx))});
        }
        lir->addOperands(Imm{0});

        break;
      }
      case Opcode::kWaitHandleLoadWaiter: {
        break;
      }
      case Opcode::kWaitHandleLoadCoroOrResult: {
        break;
      }
      case Opcode::kWaitHandleRelease: {
        break;
      }
      case Opcode::kDeleteSubscr: {
        const auto& instr = static_cast<const DeleteSubscr&>(i);
        Instruction* call = bbb.appendInstr(
            Instruction::kCall,
            OutVReg{OperandBase::k32bit},
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(PyObject_DelItem)},
            instr.GetOperand(0),
            instr.GetOperand(1));
        appendGuard(bbb, InstrGuardKind::kNotNegative, instr, call);
        break;
      }
      case Opcode::kUnpackExToTuple: {
        auto instr = static_cast<const UnpackExToTuple*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_UnpackExToTuple,
            env_->asm_tstate,
            instr->seq(),
            instr->before(),
            instr->after());
        break;
      }
      case Opcode::kReserveStack: {
        auto instr = static_cast<const ReserveStack*>(&i);
        // Reserve space in the stack frame for the stack array. The reserved
        // data is placed above the call argument buffer so that calls don't
        // clobber it, and call args remain at SP+0 where the callee expects
        // them per the ABI. The LIR ReserveStack instruction goes through
        // normal register allocation and is lowered to a LEA in autogen once
        // max_arg_buffer_size is known.
        auto aligned_size =
            static_cast<int>((instr->num_words() * kPointerSize + 15) & ~15);
        env_->reserve_stack_size =
            std::max(env_->reserve_stack_size, aligned_size);
        bbb.appendInstr(instr->output(), Instruction::kReserveStack);
        break;
      }
      case Opcode::kUnpackSequence: {
        auto instr = static_cast<const UnpackSequence*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
            JITRT_UnpackSequence,
            env_->asm_tstate,
            instr->seq(),
            instr->items_ptr(),
            instr->count());
        break;
      }
      case Opcode::kGetAIter: {
        auto& instr = static_cast<const GetAIter&>(i);
        bbb.appendCallInstruction(
            instr.output(), Ci_GetAIter, env_->asm_tstate, instr.GetOperand(0));
        break;
      }
      case Opcode::kGetANext: {
        auto& instr = static_cast<const GetAIter&>(i);
        bbb.appendCallInstruction(
            instr.output(), Ci_GetANext, env_->asm_tstate, instr.GetOperand(0));
        break;
      }
      case Opcode::kUpdatePrevInstr: {
        // We are directly referencing co_code_adaptive here rather than using
        // codeUnit() as we need to refer to the code the interpreter would
        // execute. codeUnit() returns a pointer to non-adapted bytecode.
        _Py_CODEUNIT* prev_instr_ptr;
        // We are directly referencing co_code_adaptive here rather than using
        // codeUnit() as we need to refer to the code the interpreter would
        // execute. codeUnit() returns a pointer to non-adapted bytecode.
        auto prev_instr = static_cast<const UpdatePrevInstr&>(i);
        [[maybe_unused]] Instruction* frame;
        if (prev_instr.parent() != nullptr) {
          prev_instr_ptr = i.bytecodeOffset().asIndex().value() +
              reinterpret_cast<_Py_CODEUNIT*>(
                               prev_instr.parent()->code()->co_code_adaptive);
          frame = getInlinedFrame(bbb, prev_instr.parent());
        } else {
          prev_instr_ptr = i.bytecodeOffset().asIndex().value() +
              reinterpret_cast<_Py_CODEUNIT*>(
                               func_->codeFor(i)->co_code_adaptive);
          frame = env_->asm_interpreter_frame;
        }

#if PY_VERSION_HEX >= 0x030E0000
        bbb.appendInstr(
            OutInd{frame, offsetof(_PyInterpreterFrame, instr_ptr)},
            Instruction::kMove,
            prev_instr_ptr);
#else

        bbb.appendInstr(
            OutInd{frame, offsetof(_PyInterpreterFrame, prev_instr)},
            Instruction::kMove,
            prev_instr_ptr);
#endif
        break;
      }
      case Opcode::kSend: {
        auto& hir_instr = static_cast<const Send&>(i);
        uint64_t func = reinterpret_cast<uint64_t>(
            hir_instr.handleStopAsyncIteration()
                ? JITRT_GenSendHandleStopAsyncIteration
                : JITRT_GenSend);
        appendCall2RetValues(
            bbb,
            hir_instr.output(),
            Imm{func},
            hir_instr.GetOperand(0),
            hir_instr.GetOperand(1),
            Imm{0},
            env_->asm_interpreter_frame);
        break;
      }
      case Opcode::kBuildInterpolation: {
#if PY_VERSION_HEX >= 0x030E0000
        auto& hir_instr = static_cast<const BuildInterpolation&>(i);
        bbb.appendCallInstruction(
            hir_instr.output(),
            _PyInterpolation_Build,
            hir_instr.GetOperand(0),
            hir_instr.GetOperand(1),
            hir_instr.conversion(),
            hir_instr.GetOperand(2));
#endif
        break;
      }
      case Opcode::kBuildTemplate: {
#if PY_VERSION_HEX >= 0x030E0000
        auto& hir_instr = static_cast<const BuildTemplate&>(i);
        bbb.appendInstr(
            hir_instr.output(),
            Instruction::kCall,
            Imm{reinterpret_cast<uint64_t>(_PyTemplate_Build)},
            hir_instr.GetOperand(0),
            hir_instr.GetOperand(1));
#endif
        break;
      }
      case Opcode::kLoadSpecial: {
        auto& load_special = static_cast<const LoadSpecial&>(i);
        appendCall2RetValues(
            bbb,
            load_special.output(),
            JITRT_LoadSpecial,
            load_special.GetOperand(0),
            load_special.specialIdx());
        break;
      }
      case Opcode::kConvertValue: {
#if PY_VERSION_HEX >= 0x030E0000
        auto& convert_value = static_cast<const ConvertValue&>(i);
        bbb.appendCallInstruction(
            convert_value.output(),
            _PyEval_ConversionFuncs[convert_value.converterIdx()],
            convert_value.GetOperand(0));
#endif
        break;
      }
      case Opcode::kCIntToCBool: {
        bbb.appendInstr(i.output(), Instruction::kIntToBool, i.GetOperand(0));
        break;
      }
    }

    if (auto db = i.asDeoptBase()) {
      switch (db->opcode()) {
        // These opcodes handle their own guards.
        case Opcode::kCallInd:
        case Opcode::kCheckErrOccurred:
        case Opcode::kCheckExc:
        case Opcode::kCheckField:
        case Opcode::kCheckNeg:
        case Opcode::kCheckVar:
        case Opcode::kCompareBool:
        case Opcode::kDeleteAttr:
        case Opcode::kDeleteSubscr:
        case Opcode::kDeopt:
        case Opcode::kDeoptPatchpoint:
        case Opcode::kGuard:
        case Opcode::kGuardIs:
        case Opcode::kGuardType:
        case Opcode::kInvokeStaticFunction:
        case Opcode::kIsInstance:
        case Opcode::kIsTruthy:
        case Opcode::kRaiseAwaitableError:
        case Opcode::kRaise:
        case Opcode::kRaiseStatic:
        case Opcode::kStoreAttr:
        case Opcode::kStoreAttrCached:
        case Opcode::kStoreSubscr: {
          break;
        }
        case Opcode::kPrimitiveBox: {
          auto& pb = static_cast<const PrimitiveBox&>(i);
          JIT_DCHECK(
              !(pb.value()->type() <= TCBool), "should not be able to deopt");
          emitExceptionCheck(*db, bbb);
          break;
        }
        default: {
          emitExceptionCheck(*db, bbb);
          break;
        }
      }
    }
  }

  // the last instruction must be kBranch, kCondBranch, or kReturn
  auto bbs = bbb.Generate();
  basic_blocks_.insert(basic_blocks_.end(), bbs.begin(), bbs.end());

  return {bbs.front(), bbs.back()};
}

#if defined(CINDER_AARCH64)
void LIRGenerator::updateDeoptIndex(
    BasicBlockBuilder& bbb,
    const jit::hir::Instr& i,
    jit::hir::Opcode opcode,
    int& last_deopt_line,
    const jit::hir::FrameState* caller_fs,
    BorrowedRef<PyCodeObject> inlined_code) {
  // For any instruction that can deopt, store the deopt index in the
  // outermost frame header before the instruction executes. This allows
  // frame introspection (e.g. sys._current_frames) to recover the current
  // bytecode offset without needing to walk the native stack.
  if (deopt_idx_addr_ != nullptr && i.asDeoptBase() != nullptr) {
    auto deopt_id = bbb.makeDeoptMetadata();
    bbb.appendInstr(
        OutInd{deopt_idx_addr_, 0},
        Instruction::kMove,
        Imm{deopt_id, DataType::k64bit});
    // Use inlined_code when inside an inlined function, otherwise codeFor().
    auto code = inlined_code != nullptr ? inlined_code : func_->codeFor(i);
    int bc_off = i.bytecodeOffset().value();
    last_deopt_line =
        (code != nullptr && bc_off >= 0) ? PyCode_Addr2Line(code, bc_off) : -1;
  } else if (
      deopt_idx_addr_ != nullptr &&
      (opcode == Opcode::kDecref || opcode == Opcode::kXDecref ||
       opcode == Opcode::kBatchDecref)) {
    // Decref/XDecref can run arbitrary code via __del__ but don't have
    // deopt metadata. Create an entry so that frame introspection
    // reports the correct bytecode offset / line number, but only when
    // the line differs from the last deopt point in this basic block.
    //
    // When executing inside an inlined function, we must include entries
    // for all active frames (not just the innermost) so that
    // getUnitCallStackFromDeoptIdx returns a stack matching getUnitFrames.
    // Use inlined_code when inside an inlined function, otherwise codeFor().
    auto code = inlined_code != nullptr ? inlined_code : func_->codeFor(i);
    int bc_off = i.bytecodeOffset().value();
    int decref_line =
        (code != nullptr && bc_off >= 0) ? PyCode_Addr2Line(code, bc_off) : -1;
    if (decref_line != last_deopt_line) {
      DeoptMetadata meta;
      int num_callers = 0;
      for (const auto* f = caller_fs; f != nullptr; f = f->parent) {
        num_callers++;
      }
      int num_frames = num_callers + 1;
      meta.frame_meta.resize(num_frames);
      // Innermost frame: the Decref's own code and bytecode offset.
      meta.frame_meta[num_frames - 1].code = code;
      meta.frame_meta[num_frames - 1].cause_instr_idx = i.bytecodeOffset();
      // Caller frames from the FrameState parent chain (outermost first).
      int idx = num_frames - 2;
      for (const auto* f = caller_fs; f != nullptr; f = f->parent) {
        meta.frame_meta[idx].code = f->code;
        meta.frame_meta[idx].cause_instr_idx = f->instrOffset();
        idx--;
      }
      auto deopt_id = env_->code_rt->addDeoptMetadata(std::move(meta));
      bbb.appendInstr(
          OutInd{deopt_idx_addr_, 0},
          Instruction::kMove,
          Imm{deopt_id, DataType::k64bit});
      last_deopt_line = decref_line;
    }
  }
}
#endif

void LIRGenerator::resolvePhiOperands(
    UnorderedMap<const hir::BasicBlock*, TranslatedBlock>& bb_map) {
  // This is creating a different builder than the first pass, but that's okay
  // because the state is really in `env_` which is unchanged.
  BasicBlockBuilder bbb{env_, lir_func_};

  for (auto& block : basic_blocks_) {
    block->foreachPhiInstr([&](Instruction* instr) {
      auto hir_instr = static_cast<const Phi*>(instr->origin());
      for (size_t i = 0; i < hir_instr->NumOperands(); ++i) {
        hir::BasicBlock* hir_block = hir_instr->basic_blocks().at(i);
        hir::Register* hir_value = hir_instr->GetOperand(i);
        instr->allocateLabelInput(bb_map.at(hir_block).last);
        instr->allocateLinkedInput(bbb.getDefInstr(hir_value));
      }
    });
  }
}

Instruction* LIRGenerator::getNameFromIdx(
    BasicBlockBuilder& bbb,
    const hir::DeoptBaseWithNameIdx* instr) {
  if (!getConfig().stable_frame) {
    return bbb.appendInstr(
        OutVReg{},
        Instruction::kCall,
        JITRT_LoadName,
        env_->asm_tstate,
        instr->name_idx());
  }

  BorrowedRef<PyUnicodeObject> name = instr->name();
  return bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      // TASK(T140174965): This should be MemImm.
      Imm{reinterpret_cast<uint64_t>(name.get()), OperandBase::kObject});
}

Instruction* LIRGenerator::getInlinedFrame(
    BasicBlockBuilder& bbb,
    const BeginInlinedFunction* instr) {
  auto it = env_->inline_frame_map.find(instr);
  if (it == env_->inline_frame_map.end()) {
    // In the odd case we've shuffled our basic blocks out of order and
    // encounter an inlined frame first then grab the current frame
    // offset.
    it = env_->inline_frame_map
             .emplace(
                 instr,
                 bbb.appendInstr(
                     OutVReg{},
                     Instruction::kLea,
                     Stk{PhyLocation(
                         static_cast<int32_t>(frameOffsetOf(instr)))}))
             .first;
  }

  return it->second;
}

void LIRGenerator::emitLoadFrame(BasicBlockBuilder& bbb) {
  // For non-generator normal (heavy) frames, allocate and link the
  // interpreter frame via a runtime call. This moves the work that
  // was previously done in frame_asm.cpp's linkNormalFunctionFrame
  // into LIR so the register allocator can optimize it.
  bool is_gen =
      func_->code != nullptr && (func_->code->co_flags & kCoFlagsAnyGenerator);
  if (is_gen) {
    // Load the resume entry label address.
    bbb.annotateNext("Allocate generator + interpreter frame");
    Instruction* resume_label = bbb.appendInstr(
        OutVReg{}, Instruction::kLea, AsmLbl{env_->gen_resume_entry_label});
    // spill_words is read from CodeRuntime by the runtime function,
    // so we don't need to pass it explicitly.
    Instruction* footer;
    appendCall2RetValues(
        bbb,
        env_->asm_tstate,
        footer,
        JITRT_AllocateAndLinkGenAndInterpreterFrame,
        env_->asm_func,
        Imm{reinterpret_cast<uint64_t>(env_->code_rt)},
        VReg{resume_label},
        PhyReg{codegen::arch::reg_frame_pointer_loc});
    // Swap RBP to point at the generator data so spills go there.
    bbb.annotateNext("Set frame pointer to GenDataFooter");
    bbb.appendInstr(
        OutPhyReg{codegen::arch::reg_frame_pointer_loc},
        Instruction::kMove,
        VReg{footer});
#if defined(CINDER_AARCH64) && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    // Now that FP points at the heap-allocated GenDataFooter, compute the
    // deopt_idx address.  This must happen after the FP swap above —
    // the kLea uses FP as its base register.
    // TranslateOneBasicBlock reuses this for all deopt index stores.
    if (getConfig().frame_mode == FrameMode::kLightweight) {
      int32_t deopt_idx_offset = static_cast<int32_t>(
          offsetof(GenDataFooter, frame_header) +
          offsetof(FrameHeader, deopt_idx));
      deopt_idx_addr_ = bbb.appendInstr(
          OutVReg{}, Instruction::kLea, Stk{PhyLocation(deopt_idx_offset)});
    }
#endif
  } else if (func_->frameMode == FrameMode::kNormal) {
    bbb.annotateNext("Allocate and link interpreter frame");
    if (kPyDebug) {
      env_->asm_tstate = bbb.appendCallInstruction(
          OutVReg{},
          JITRT_AllocateAndLinkInterpreterFrame_Debug,
          env_->asm_func,
          func_->code.get());
    } else {
      env_->asm_tstate = bbb.appendCallInstruction(
          OutVReg{},
          JITRT_AllocateAndLinkInterpreterFrame_Release,
          env_->asm_func);
    }
  }
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  else if (func_->frameMode == FrameMode::kLightweight) {
#if defined(CINDER_AARCH64) && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    // Compute the address of the deopt_idx field once
    // TranslateOneBasicBlock reuses this for all deopt index stores.
    if (func_->code != nullptr) {
      int32_t deopt_idx_offset = static_cast<int32_t>(
          -(Py_ssize_t)frameHeaderSize(func_->code) +
          offsetof(FrameHeader, deopt_idx));
      auto* instr = bbb.appendInstr(Instruction::kLea);
      instr->output()->setVirtualRegister();
      instr->allocateStackInput(PhyLocation(deopt_idx_offset));
      deopt_idx_addr_ = instr;
    }
#endif

    // Lightweight frame linking: populate all _PyInterpreterFrame
    // fields inline, following the BeginInlinedFunction pattern.
    bbb.annotateNext("Lightweight frame: load thread state");
    env_->asm_tstate =
        bbb.appendInstr(OutVReg{}, Instruction::kLoadThreadState);

    int fh_size = jit::frameHeaderSize(func_->code);
    // The _PyInterpreterFrame starts after the FrameHeader.
    Instruction* frame = bbb.appendInstr(
        OutVReg{},
        Instruction::kLea,
        Stk{PhyLocation(
            static_cast<int32_t>(-fh_size + sizeof(jit::FrameHeader)))});

    // Store FrameHeader
    bbb.annotateNext("Lightweight frame: store frame header");
#if PY_VERSION_HEX >= 0x030E0000
    // Store rtfs = 0 in FrameHeader
    bbb.appendInstr(
        OutInd{
            frame,
            static_cast<Py_ssize_t>(offsetof(jit::FrameHeader, func)) -
                static_cast<Py_ssize_t>(sizeof(jit::FrameHeader))},
        Instruction::kMove,
        Imm{0});
#else
    // Store func in FrameHeader (with incref)
    Instruction* func_for_header =
        bbb.appendInstr(OutVReg{}, Instruction::kMove, env_->asm_func);
    bbb.appendInstr(
        OutInd{
            frame,
            static_cast<Py_ssize_t>(offsetof(jit::FrameHeader, func)) -
                static_cast<Py_ssize_t>(sizeof(jit::FrameHeader))},
        Instruction::kMove,
        func_for_header);
    makeIncref(bbb, func_for_header, false);
#endif

    // Store f_executable
    bbb.annotateNext("Set _PyInterpreterFrame::f_executable/f_code");
#if PY_VERSION_HEX >= 0x030E0000
    BorrowedRef<> reifier = env_->code_rt->reifier();
    PyObject* executable = reifier.get();
    if (executable == nullptr) {
      executable = reinterpret_cast<PyObject*>(func_->code.get());
    }
#else
    PyObject* executable = reinterpret_cast<PyObject*>(func_->code.get());
#endif
    Instruction* executable_reg =
        bbb.appendInstr(OutVReg{}, Instruction::kMove, executable);
    bbb.appendInstr(
        OutInd{frame, FRAME_EXECUTABLE_OFFSET},
        Instruction::kMove,
        executable_reg);
    if (!_Py_IsImmortal(executable)) {
      makeIncref(bbb, executable_reg, false);
    }

    // Store f_funcobj
    bbb.annotateNext("Set _PyInterpreterFrame::f_funcobj");
#if PY_VERSION_HEX >= 0x030E0000
    // 3.14+: f_funcobj = func (with incref)
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, f_funcobj)},
        Instruction::kMove,
        env_->asm_func);
    makeIncref(bbb, env_->asm_func, false);
#else
    // 3.12-3.13: f_funcobj = frame_reifier (immortal)
    PyObject* frame_reifier = cinderx::getModuleState()->frame_reifier;
    Instruction* reifier_reg =
        bbb.appendInstr(OutVReg{}, Instruction::kMove, frame_reifier);
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, f_funcobj)},
        Instruction::kMove,
        reifier_reg);
#endif

    // Store previous = tstate->current_frame
    bbb.annotateNext("Set _PyInterpreterFrame::previous");
#if PY_VERSION_HEX >= 0x030D0000
    Instruction* prev_frame = bbb.appendInstr(
        OutVReg{},
        Instruction::kMove,
        Ind{env_->asm_tstate, offsetof(PyThreadState, current_frame)});
#else
    Instruction* cframe_reg = bbb.appendInstr(
        OutVReg{},
        Instruction::kMove,
        Ind{env_->asm_tstate, offsetof(PyThreadState, cframe)});
    Instruction* prev_frame = bbb.appendInstr(
        OutVReg{},
        Instruction::kMove,
        Ind{cframe_reg, offsetof(_PyCFrame, current_frame)});
#endif
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, previous)},
        Instruction::kMove,
        prev_frame);

    // Store prev_instr / instr_ptr
    bbb.annotateNext("Set _PyInterpreterFrame::prev_instr");
#if PY_VERSION_HEX >= 0x030E0000
    _Py_CODEUNIT* code_start = _PyCode_CODE(func_->code.get());
#else
    _Py_CODEUNIT* code_start = _PyCode_CODE(func_->code.get()) - 1;
#endif
    Instruction* code_start_reg =
        bbb.appendInstr(OutVReg{}, Instruction::kMove, code_start);
    bbb.appendInstr(
        OutInd{frame, FRAME_INSTR_OFFSET}, Instruction::kMove, code_start_reg);

#ifdef Py_GIL_DISABLED
    bbb.annotateNext("Set _PyInterpreterFrame::store tlbc_index");
    // Store tlbc_index = 0
    bbb.appendInstr(
        OutInd{
            frame,
            offsetof(_PyInterpreterFrame, tlbc_index),
            OperandBase::k32bit},
        Instruction::kMove,
        Imm{0, OperandBase::k32bit});
#endif

    // Store owner = FRAME_OWNED_BY_THREAD
    bbb.annotateNext("Set _PyInterpreterFrame::owner");
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, owner), OperandBase::k8bit},
        Instruction::kMove,
        Imm{static_cast<uint8_t>(FRAME_OWNED_BY_THREAD), OperandBase::k8bit});

#if PY_VERSION_HEX >= 0x030E0000
    bbb.annotateNext("Set _PyInterpreterFrame::localsplus");
    // Store stackpointer = &localsplus[0]
    Instruction* localsplus = bbb.appendInstr(
        OutVReg{},
        Instruction::kLea,
        Stk{PhyLocation(
            static_cast<int32_t>(
                -fh_size + sizeof(jit::FrameHeader) +
                offsetof(_PyInterpreterFrame, localsplus)))});
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, stackpointer)},
        Instruction::kMove,
        localsplus);

    // Store f_locals = NULL
    bbb.annotateNext("Set _PyInterpreterFrame::f_locals");
    bbb.appendInstr(
        OutInd{frame, offsetof(_PyInterpreterFrame, f_locals)},
        Instruction::kMove,
        Imm{0});
#endif

    // Link frame into tstate
    bbb.annotateNext("Set _PyInterpreterFrame as topmost frame");
#if PY_VERSION_HEX >= 0x030D0000
    bbb.appendInstr(
        OutInd{env_->asm_tstate, offsetof(PyThreadState, current_frame)},
        Instruction::kMove,
        frame);
#else
    bbb.appendInstr(
        OutInd{cframe_reg, offsetof(_PyCFrame, current_frame)},
        Instruction::kMove,
        frame);
#endif
  }
#endif // ENABLE_LIGHTWEIGHT_FRAMES
#if PY_VERSION_HEX >= 0x030D0000
  bbb.annotateNext("Load current interpreter frame");
  env_->asm_interpreter_frame = bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      Ind{env_->asm_tstate, offsetof(PyThreadState, current_frame)});
#else
  auto* cframe = bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      Ind{env_->asm_tstate, offsetof(PyThreadState, cframe)});
  env_->asm_interpreter_frame = bbb.appendInstr(
      OutVReg{},
      Instruction::kMove,
      Ind{cframe, offsetof(_PyCFrame, current_frame)});
#endif
}

void GenerateDeoptTrampolineBlocks(
    Function* lir_func,
    bool generator_mode,
    void* prepare_for_deopt,
    void* resume_in_interpreter) {
  namespace arch = codegen::arch;

  auto* block = lir_func->allocateBasicBlock();

  emitAnnotation(block, "Save registers");

  // Stage 1: Save all GP registers via VariadicPush.
  // Each architecture saves all GP registers except the deopt scratch register
  // (already saved by the stage 2 stub).
#if defined(CINDER_X86_64)
  constexpr int kStage3SavedRegs = 15;
  constexpr int kStage2SavedRegs = 1; // r15
#elif defined(CINDER_AARCH64)
  constexpr int kStage3SavedRegs = 28;
  constexpr int kStage2SavedRegs = 2; // x28 + fp
#endif

  auto* vpush = block->allocateInstr(Instruction::kVariadicPush, nullptr);
  // Push in descending order so the memory layout is ascending by PhyLocation.
  // r15 was already saved by stage 2.
  for (int i = 0; i < kStage3SavedRegs; i++) {
    vpush->addOperands(PhyReg{PhyLocation{i}});
  }

  // Shared computation of stack layout constants.
  constexpr int kMetadataSlots = 4;
  constexpr int total_gp_regs = kStage3SavedRegs + kStage2SavedRegs;
  constexpr int frame_offset = (total_gp_regs + kMetadataSlots) * kPointerSize;
  // cleanup_size skips all stage 3 regs; the stage 2 scratch reg is
  // loaded separately in stage 4.
  constexpr int cleanup_size = kStage3SavedRegs * kPointerSize;

  // Generate the stage 2 trampoline (one per function). This saves the address
  // of the final part of the JIT-epilogue that is responsible for restoring
  // callee-saved registers and returning, our scratch register, whose original
  // contents may be needed during frame reification, and jumps to the final
  // trampoline.
  //
  // Right now the top of the stack looks like:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved pc/rip            | <-- sp
  // +-------------------------+
  //
  // and we need to pass our scratch register and the address of the epilogue
  // to the global deopt trampoline. The code below leaves the stack with the
  // following layout:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip/pc            |
  // | padding                 |
  // | padding                 |
  // | address of CodeRuntime  |
  // | address of epilogue     |
  // | [fp on arm]             |
  // | x28/r15                 |
  // +-------------------------+
  //
  // The global deopt trampoline expects that our scratch register is at the
  // top of the stack so that it can save the remaining registers immediately
  // after it, forming a contiguous array of all registers.
  //
  // If you change this make sure you update that code!
  emitAnnotation(block, "Shuffle rip, rbp, and deopt index");

  constexpr auto sp_reg = codegen::arch::reg_stack_pointer_loc;
  constexpr auto fp_reg = codegen::arch::reg_frame_pointer_loc;
  // The frame setup doubles as prepareForDeopt's call args: arg0 = &regs,
  // arg1 = code_rt, arg2 = deopt_idx.  saved_rip_reg is just a scratch.
  constexpr auto arg0_reg = codegen::ARGUMENT_REGS[0];
  constexpr auto code_rt_reg = codegen::ARGUMENT_REGS[1];
  constexpr auto deopt_idx_reg = codegen::ARGUMENT_REGS[2];
  constexpr auto saved_rip_reg = codegen::ARGUMENT_REGS[3];

  // arg0 = &saved_regs (sp value)
  block->allocateInstr(
      Instruction::kLea, nullptr, OutPhyReg{arg0_reg}, Ind(sp_reg, 0));

  // Load saved rip/pc from [sp + frame_offset] into scratch.
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{saved_rip_reg},
      Ind(sp_reg, frame_offset));

  // For generators, restore original frame pointer while fp still points
  // to the generator data footer.
  if (generator_mode) {
    constexpr auto orig_fp_off =
        static_cast<int32_t>(offsetof(GenDataFooter, originalFramePointer));
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{fp_reg},
        Ind(fp_reg, orig_fp_off));
  }

  // Save fp (now the original for generators) to [sp + frame_offset].
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, frame_offset),
      PhyReg{fp_reg});

  // Set up our frame: fp = sp + frame_offset.
  block->allocateInstr(
      Instruction::kLea, nullptr, OutPhyReg{fp_reg}, Ind(sp_reg, frame_offset));

  // Load deopt_idx from [fp + 8] (the slot above saved_rbp).
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{deopt_idx_reg},
      Ind(fp_reg, kPointerSize));

  // Store saved rip/pc to [fp + 8] (restoring the real return address).
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(fp_reg, kPointerSize),
      PhyReg{saved_rip_reg});

  // Store deopt_idx to [fp - 16] (a padding slot for later retrieval).
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(fp_reg, -2 * kPointerSize),
      PhyReg{deopt_idx_reg});

  // Load code_rt from [fp - 24] (stashed by stage 2).
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{code_rt_reg},
      Ind(fp_reg, -3 * kPointerSize));

  // Stage 3: Call prepareForDeopt.
  // Returns a packed uintptr_t in RAX/X0: frame pointer with
  // is_instrumentation_deopt encoded in bit 0.
  emitAnnotation(block, "prepareForDeopt");

  // Windows x64: reserve shadow space for the callee.
  if constexpr (
      arch::kBuildArch == arch::Arch::kX86_64 &&
      codegen::kShadowSpaceSize > 0) {
    block->allocateInstr(
        Instruction::kLea,
        nullptr,
        OutPhyReg{sp_reg},
        Ind(sp_reg, -codegen::kShadowSpaceSize));
  }

  block->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm{reinterpret_cast<uint64_t>(prepare_for_deopt)});

  // Windows x64: free shadow space.
  if constexpr (
      arch::kBuildArch == arch::Arch::kX86_64 &&
      codegen::kShadowSpaceSize > 0) {
    block->allocateInstr(
        Instruction::kLea,
        nullptr,
        OutPhyReg{sp_reg},
        Ind(sp_reg, codegen::kShadowSpaceSize));
  }

  // Stage 4: Clean up saved registers + restore deopt scratch reg.
  emitAnnotation(block, "reg cleanup");
  // Load the deopt scratch register from its saved position on the stack.
  constexpr auto scratch_deopt_reg = codegen::arch::reg_scratch_deopt_loc;
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{scratch_deopt_reg},
      Ind(sp_reg, cleanup_size));
  // Adjust SP past all saved registers (stage 3 regs + stage 2 regs).
  block->allocateInstr(
      Instruction::kLea,
      nullptr,
      OutPhyReg{sp_reg},
      Ind(sp_reg, cleanup_size + kStage2SavedRegs * kPointerSize));

  // Stage 5: Call resumeInInterpreter.
  //
  // prepareForDeopt returned a packed uintptr_t in the return register:
  //   bit 0       = is_instrumentation_deopt  (arg3)
  //   bits 63..1  = frame pointer             (arg0)
  // We unpack before setting up the remaining arguments.
  emitAnnotation(block, "resumeInInterpreter");

  constexpr auto ret_loc = codegen::arch::reg_general_return_loc;

  // Extract is_instrumentation_deopt (bit 0) into arg3 FIRST, before we
  // mask the return register to get the frame pointer.
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{codegen::ARGUMENT_REGS[3]},
      PhyReg{ret_loc});
  block->allocateInstr(
      Instruction::kAnd,
      nullptr,
      OutPhyReg{codegen::ARGUMENT_REGS[3]},
      PhyReg{codegen::ARGUMENT_REGS[3]},
      Imm{1});

  // Clear bit 0 to recover the frame pointer.
  block->allocateInstr(
      Instruction::kAnd,
      nullptr,
      OutPhyReg{ret_loc},
      PhyReg{ret_loc},
      Imm{~static_cast<uint64_t>(1)});

  // arg0 = frame pointer (now clean in the return register).
  if (codegen::ARGUMENT_REGS[0] != ret_loc) {
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{codegen::ARGUMENT_REGS[0]},
        PhyReg{ret_loc});
  }
  // arg1 = code_rt from stack (fp - 3*8)
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{codegen::ARGUMENT_REGS[1]},
      Ind(fp_reg, -3 * kPointerSize));
  // arg2 = deopt_idx from stack (fp - 2*8)
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{codegen::ARGUMENT_REGS[2]},
      Ind(fp_reg, -2 * kPointerSize));

  // Windows x64: reserve shadow space for the callee.
  if constexpr (
      arch::kBuildArch == arch::Arch::kX86_64 &&
      codegen::kShadowSpaceSize > 0) {
    block->allocateInstr(
        Instruction::kLea,
        nullptr,
        OutPhyReg{sp_reg},
        Ind(sp_reg, -codegen::kShadowSpaceSize));
  }

  block->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm{reinterpret_cast<uint64_t>(resume_in_interpreter)});
  // Shadow space (if any) is freed by kLeave below which restores RSP
  // from RBP.

  // Stage 6: Exit — copy result to error-signal registers, load epilogue
  // address, tear down frame, jump to real epilogue.
  emitAnnotation(block, "Jump to real epilogue");

  // Return value register (64-bit) and its 32-bit alias.
  constexpr auto ret_reg = codegen::arch::reg_general_return_loc;
  constexpr PhyLocation ret32_reg{ret_reg.loc, 32};
  // Auxiliary return registers (int32 + double) for primitive error signal.
  constexpr auto aux_ret_reg = codegen::arch::reg_general_auxilary_return_loc;
  constexpr PhyLocation err32_reg{aux_ret_reg.loc, 32};
  constexpr auto err_xmm_reg = codegen::arch::reg_double_auxilary_return_loc;
#if defined(CINDER_X86_64)
  constexpr auto jump_target_reg = codegen::ARGUMENT_REGS[0]; // scratch
#elif defined(CINDER_AARCH64)
  constexpr auto jump_target_reg = codegen::arch::reg_scratch_br_loc; // X16
#endif

  // Copy return value to primitive-error signal registers.
  block->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg{err32_reg}, PhyReg{ret32_reg});
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{err_xmm_reg, OperandBase::kDouble},
      PhyReg{ret_reg});
  // Load epilogue address (must be before kLeave which tears down the frame).
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{jump_target_reg},
      Ind(fp_reg, -4 * kPointerSize));
  // Tear down the frame.
  block->allocateInstr(Instruction::kLeave, nullptr);
#if defined(CINDER_X86_64)
  // Skip the saved rip slot (normally consumed by ret, but we jump instead).
  block->allocateInstr(
      Instruction::kLea, nullptr, OutPhyReg{sp_reg}, Ind(sp_reg, kPointerSize));
#endif
  // Jump to the real epilogue.
  block->allocateInstr(
      Instruction::kIndirectJump, nullptr, PhyReg{jump_target_reg});
}

void GenerateFailedDeferredCompileBlocks(
    Function* lir_func,
    void* failed_deferred_compile_shim) {
  auto* block = lir_func->allocateBasicBlock();

  // Set up a frame.
  block->allocateInstr(Instruction::kPrologue, nullptr);

  // Save incoming argument registers.
  emitAnnotation(block, "saveRegisters");
  auto* vpush = block->allocateInstr(Instruction::kVariadicPush, nullptr);
  for (int i = 0; i < codegen::ARGUMENT_REGS.size(); i++) {
    vpush->addOperands(PhyReg{codegen::ARGUMENT_REGS[i]});
  }

  // arg0 = pointer to saved argument registers on the stack.
  constexpr auto sp_reg = codegen::arch::reg_stack_pointer_loc;
  block->allocateInstr(
      Instruction::kLea,
      nullptr,
      OutPhyReg{codegen::ARGUMENT_REGS[0]},
      Ind(sp_reg, 0));

  // Call JITRT_FailedDeferredCompileShim.
  block->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm{reinterpret_cast<uint64_t>(failed_deferred_compile_shim)});

  // Tear down the frame and return.
  block->allocateInstr(Instruction::kLeave, nullptr);
  block->allocateInstr(Instruction::kRet, nullptr);
}

void GenerateDeoptExitBlocks(Function* lir_func, jit::codegen::Environ* env) {
  // Collect deopt metadata IDs and the HIR origin of each guard/patchpoint
  // instruction. The origin is needed so the stage 1 call records a debug
  // entry at the return address (used by deoptAllJitFramesOnStack's IP-based
  // lookup on x86).
  struct DeoptEntry {
    size_t id;
    const hir::Instr* origin;
  };
  std::vector<DeoptEntry> deopt_entries;
  for (auto* bb : lir_func->basicblocks()) {
    for (auto& instr : bb->instructions()) {
      if (instr->isGuard() || instr->isDeoptPatchpoint()) {
        size_t deopt_id =
            static_cast<size_t>(instr->getInput(1)->getConstant());
        deopt_entries.push_back({deopt_id, instr->origin()});
      }
    }
  }

  if (deopt_entries.empty()) {
    return;
  }

  // Sort and deduplicate by ID for deterministic code layout.
  std::sort(deopt_entries.begin(), deopt_entries.end(), [](auto& a, auto& b) {
    return a.id < b.id;
  });
  deopt_entries.erase(
      std::unique(
          deopt_entries.begin(),
          deopt_entries.end(),
          [](auto& a, auto& b) { return a.id == b.id; }),
      deopt_entries.end());

  // Create stage 1 blocks first (one per deopt point), then stage 2 last.
  // This gives the natural layout: stage 1 blocks followed by stage 2.

  // Pre-create the stage 2 block pointer so stage 1 can reference it.
  // We'll actually allocate it after stage 1 blocks so it appears last.
  BasicBlock* stage2_block = nullptr;

  // Create stage 1 blocks (per guard, cold section).
  for (const auto& entry : deopt_entries) {
    auto* stage1_block = lir_func->allocateBasicBlock();
    stage1_block->setSection(codegen::CodeSection::kCold);

    // The deopt metadata index is pushed/stored so the global trampoline
    // can look up the DeoptMetadata.
#if defined(CINDER_X86_64)
    // push deopt_meta_index; call stage2
    // The call pushes the return address (= point after the guard).
    stage1_block->allocateInstr(Instruction::kPush, nullptr, Imm{entry.id});
#elif defined(CINDER_AARCH64)
    // mov x13, deopt_meta_index; bl stage2
    // bl captures the return address in LR.
    stage1_block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{codegen::arch::reg_scratch_0_loc},
        Imm{entry.id});
#endif

    // The kCall references the stage 2 block (filled in below). The origin
    // is set so autogen records a debug entry at the return address — needed
    // by deoptAllJitFramesOnStack's IP-based frame lookup on x86.
    stage1_block->allocateInstr(Instruction::kCall, entry.origin);

    env->deopt_exit_blocks[entry.id] = stage1_block;
  }

  // Create stage 2 block (per function, cold section).
  // Saves the deopt scratch register, frame pointer, CodeRuntime address, and
  // epilogue address on the stack, then jumps to the global deopt trampoline.
  stage2_block = lir_func->allocateBasicBlock();
  stage2_block->setSection(codegen::CodeSection::kCold);

  emitAnnotation(stage2_block, "Deoptimization exits");

  constexpr auto scratch_deopt = codegen::arch::reg_scratch_deopt_loc;
  constexpr auto sp_reg = codegen::arch::reg_stack_pointer_loc;

  // Stage 2 stack frame layout (low address → high):
  //   x86:     [r15]      [epilogue] [code_rt] [pad] [pad]            = 5
  //   aarch64: [x28] [fp] [epilogue] [code_rt] [pad] [pad] [pc] [idx] = 8
  //
  // On x86, stage 1 pushes the deopt index and `call` pushes the return
  // address, so stage 2 only needs 5 slots.  On aarch64, `bl` captures the
  // return address in LR and the deopt index is in x13, so stage 2 must
  // store both explicitly in the last 2 slots.
#if defined(CINDER_X86_64)
  constexpr int kStage2Slots = 5;
  constexpr int kCodeRtOffset = 2 * kPointerSize;
  constexpr int kEpilogueOffset = 1 * kPointerSize;
#elif defined(CINDER_AARCH64)
  constexpr int kStage2Slots = 8;
  constexpr int kCodeRtOffset = 3 * kPointerSize;
  constexpr int kEpilogueOffset = 2 * kPointerSize;
  constexpr int kSavedPcOffset = 6 * kPointerSize;
  constexpr int kDeoptIdxOffset = 7 * kPointerSize;
#endif

  // --- Platform-specific stack allocation and register saves ---
#if defined(CINDER_X86_64)
  // Push 5 slots (saved r15, epilogue, code_rt, padding x2).
  // One padding slot will get the deopt metadata index shuffled in by the
  // global trampoline.
  for (int i = 0; i < kStage2Slots; i++) {
    stage2_block->allocateInstr(
        Instruction::kPush, nullptr, PhyReg{scratch_deopt});
  }
#elif defined(CINDER_AARCH64)
  constexpr auto fp_reg = codegen::arch::reg_frame_pointer_loc;

  // Allocate 8 slots. Store x28, fp, and the stage 1 values (LR, x13)
  // that on x86 would have been pushed by stage 1 / the call instruction.
  stage2_block->allocateInstr(
      Instruction::kLea,
      nullptr,
      OutPhyReg{sp_reg},
      Ind(sp_reg, -kStage2Slots * kPointerSize));
  stage2_block->allocateInstr(
      Instruction::kMove, nullptr, OutInd(sp_reg, 0), PhyReg{scratch_deopt});
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, kPointerSize),
      PhyReg{fp_reg});
  // Store return address (LR) and deopt metadata index (x13) into the
  // slots that correspond to the stage 1 push on x86.
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, kSavedPcOffset),
      PhyReg{codegen::X30});
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, kDeoptIdxOffset),
      PhyReg{codegen::arch::reg_scratch_0_loc});
#endif

  // --- Shared: store CodeRuntime and epilogue addresses, jump to trampoline
  // ---

  // Store CodeRuntime address.
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{scratch_deopt},
      Imm{reinterpret_cast<uint64_t>(env->code_rt)});
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, kCodeRtOffset),
      PhyReg{scratch_deopt});

  // Store epilogue address.
  stage2_block->allocateInstr(
      Instruction::kLea,
      nullptr,
      OutPhyReg{scratch_deopt},
      AsmLbl{env->hard_exit_label});
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd(sp_reg, kEpilogueOffset),
      PhyReg{scratch_deopt});

  // Jump to the global deopt trampoline.
  stage2_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{scratch_deopt},
      Imm{reinterpret_cast<uint64_t>(env->deopt_trampoline)});
  stage2_block->allocateInstr(
      Instruction::kIndirectJump, nullptr, PhyReg{scratch_deopt});

  // Now fill in the kCall label operands in each stage 1 block.
  for (auto& [deopt_id, stage1_block] : env->deopt_exit_blocks) {
    auto* call_instr = stage1_block->getLastInstr();
    JIT_CHECK(call_instr->isCall(), "Expected kCall as last instruction");
    call_instr->allocateLabelInput(stage2_block);
  }
}

} // namespace jit::lir
