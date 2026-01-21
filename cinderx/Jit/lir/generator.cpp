// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/generator.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_shadow_frame.h"
#else
#include "internal/pycore_ceval.h"
#include "internal/pycore_intrinsics.h"
#endif

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

#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/iter_helpers.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/block_builder.h"
#include "cinderx/Jit/runtime_support.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <functional>
#include <sstream>

// XXX: this file needs to be revisited when we optimize HIR-to-LIR translation
// in codegen.cpp/h. Currently, this file is almost an identical copy from
// codegen.cpp with some interfaces changes so that it works with the new
// LIR.

using namespace jit::hir;

namespace jit::lir {

namespace {

constexpr size_t kRefcountOffset = offsetof(PyObject, ob_refcnt);

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

ssize_t frameOffsetBefore(const BeginInlinedFunction* instr) {
#if PY_VERSION_HEX < 0x030C0000
  return -instr->inlineDepth() * ssize_t{kJITShadowFrameSize};
#else
  ssize_t depth = 0;
  for (auto frame = instr->callerFrameState(); frame != nullptr;
       frame = frame->parent) {
    depth -= frameHeaderSize(frame->code);
  }
  return depth;
#endif
}

ssize_t frameOffsetOf(const BeginInlinedFunction* instr) {
#if PY_VERSION_HEX < 0x030C0000
  return frameOffsetBefore(instr) - ssize_t{kJITShadowFrameSize};
#else
  return frameOffsetBefore(instr) - frameHeaderSize(instr->code());
#endif
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
    : func_(func), env_(env) {
  for (int i = 0, n = func->env.numLoadTypeAttrCaches(); i < n; i++) {
    load_type_attr_caches_.emplace_back(
        Runtime::get()->allocateLoadTypeAttrCache());
  }
  for (int i = 0, n = func->env.numLoadTypeMethodCaches(); i < n; i++) {
    load_type_method_caches_.emplace_back(
        Runtime::get()->allocateLoadTypeMethodCache());
  }
}

BasicBlock* LIRGenerator::GenerateEntryBlock() {
  auto block = lir_func_->allocateBasicBlock();
  auto bindVReg = [&](PhyLocation phy_reg) {
    auto instr = block->allocateInstr(Instruction::kBind, nullptr);
    instr->output()->setVirtualRegister();
    instr->allocatePhyRegisterInput(phy_reg);
    return instr;
  };

  env_->asm_extra_args = bindVReg(codegen::INITIAL_EXTRA_ARGS_REG);
  env_->asm_tstate = bindVReg(codegen::INITIAL_TSTATE_REG);
  if (func_->uses_runtime_func) {
    env_->asm_func = bindVReg(codegen::INITIAL_FUNC_REG);
  }

#if PY_VERSION_HEX >= 0x030C0000

  // Load the current interpreter frame pointer from tstate.

#if PY_VERSION_HEX >= 0x030D0000
  env_->asm_interpreter_frame = block->allocateInstr(
      Instruction::kMove,
      nullptr /* HIR instruction */,
      OutVReg{},
      Ind{env_->asm_tstate, offsetof(PyThreadState, current_frame)});
#else
  Instruction* cframe = block->allocateInstr(
      Instruction::kMove,
      nullptr /* HIR instruction */,
      OutVReg{},
      Ind{env_->asm_tstate, offsetof(PyThreadState, cframe)});
  env_->asm_interpreter_frame = block->allocateInstr(
      Instruction::kMove,
      nullptr /* HIR instruction */,
      OutVReg{},
      Ind{cframe, offsetof(_PyCFrame, current_frame)});
#endif

#endif

  return block;
}

BasicBlock* LIRGenerator::GenerateExitBlock() {
  return lir_func_->allocateBasicBlock();
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

  UnorderedMap<const hir::BasicBlock*, TranslatedBlock> bb_map;
  std::vector<const hir::BasicBlock*> translated;
  auto translate_block = [&](const hir::BasicBlock* hir_bb) {
    bb_map.emplace(hir_bb, TranslateOneBasicBlock(hir_bb));
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

  exit_block_ = GenerateExitBlock();

  // Connect all successors.
  entry_block_->addSuccessor(bb_map[hir_entry].first);
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
        break;
      }
      default:
        break;
    }
  }

  resolvePhiOperands(bb_map);

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

  if (callee == cinderx::getModuleState()->builtinNext()) {
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

void LIRGenerator::MakeIncref(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    bool xincref,
    bool possible_immortal) {
  auto end_incref = bbb.allocateBlock();
  if (xincref) {
    auto cont = bbb.allocateBlock();
    bbb.appendBranch(Instruction::kCondBranch, instr, cont, end_incref);
    bbb.appendBlock(cont);
  }

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

  bbb.appendBlock(end_incref);
}

void LIRGenerator::MakeIncref(
    BasicBlockBuilder& bbb,
    const hir::Instr& instr,
    bool xincref) {
  Register* obj = instr.GetOperand(0);

  // Don't generate anything for immortal objects.
  if (kImmortalInstances && !obj->type().couldBe(TMortalObject)) {
    return;
  }

  MakeIncref(
      bbb,
      bbb.getDefInstr(obj),
      xincref,
      kImmortalInstances && obj->type().couldBe(TImmortalObject));
}

void LIRGenerator::MakeDecref(
    BasicBlockBuilder& bbb,
    lir::Instruction* instr,
    std::optional<destructor> destructor,
    bool xdecref,
    bool possible_immortal) {
  auto end_decref = bbb.allocateBlock();
  if (xdecref) {
    auto cont = bbb.allocateBlock();
    bbb.appendBranch(Instruction::kCondBranch, instr, cont, end_decref);
    bbb.appendBlock(cont);
  }

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

  bbb.appendBlock(end_decref);
}

void LIRGenerator::MakeDecref(
    BasicBlockBuilder& bbb,
    const jit::hir::Instr& instr,
    bool xdecref) {
  Register* obj = instr.GetOperand(0);

  // Don't generate anything for immortal objects.
  if (kImmortalInstances && !obj->type().couldBe(TMortalObject)) {
    return;
  }

  MakeDecref(
      bbb,
      bbb.getDefInstr(obj),
      obj->type().runtimePyTypeDestructor(),
      xdecref,
      kImmortalInstances && obj->type().couldBe(TImmortalObject));
}

LIRGenerator::TranslatedBlock LIRGenerator::TranslateOneBasicBlock(
    const hir::BasicBlock* hir_bb) {
  BasicBlockBuilder bbb{env_, lir_func_};
  BasicBlock* entry_block = bbb.allocateBlock();
  bbb.switchBlock(entry_block);

  for (auto& i : *hir_bb) {
    auto opcode = i.opcode();
    bbb.setCurrentInstr(&i);
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
      case Opcode::kMakeCell: {
        auto instr = static_cast<const MakeCell*>(&i);
        bbb.appendCallInstruction(
            instr->output(), PyCell_New, instr->GetOperand(0));
        break;
      }
      case Opcode::kStealCellItem:
      case Opcode::kLoadCellItem: {
        hir::Register* dest = i.output();
        Instruction* src_base = bbb.getDefInstr(i.GetOperand(0));
        constexpr int32_t kOffset = offsetof(PyCellObject, ob_ref);
        bbb.appendInstr(dest, Instruction::kMove, Ind{src_base, kOffset});
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
#if PY_VERSION_HEX >= 0x030C0000
        auto& hir_instr = static_cast<const InitFrameCellVars&>(i);
        bbb.appendInvokeInstruction(
            JITRT_InitFrameCellVars,
            bbb.getDefInstr(hir_instr.func()),
            hir_instr.num_cell_vars(),
            env_->asm_tstate);
#else
        JIT_CHECK(false, "kInitFrameCellVars is only 3.12 and later");
#endif
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
      case Opcode::kIntConvert: {
        auto instr = static_cast<const IntConvert*>(&i);
        if (instr->type() <= TCBool) {
          bbb.appendInstr(instr->output(), Instruction::kMove, instr->src());
        } else if (instr->type() <= TCUnsigned) {
          bbb.appendInstr(instr->output(), Instruction::kZext, instr->src());
        } else {
          JIT_CHECK(
              instr->type() <= TCSigned,
              "Unexpected IntConvert type {}",
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
          bbb.appendCallInstruction(
              instr->output(),
              JITRT_PowerDouble,
              instr->left(),
              instr->right());
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

        constexpr int32_t kOffset = offsetof(
            PyThreadState,
#if PY_VERSION_HEX >= 0x030C0000
            current_exception
#else
            curexc_type
#endif
        );
        Instruction* curexc = bbb.appendInstr(
            Instruction::kMove, OutVReg{}, Ind{env_->asm_tstate, kOffset});

        Instruction* is_no_err_set = bbb.appendInstr(
            Instruction::kEqual,
            OutVReg{OperandBase::k8bit},
            curexc,
            MemImm{nullptr});

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
        bbb.appendInstr(Instruction::kReturn, i.GetOperand(0));
        break;
      }
      case Opcode::kSetCurrentAwaiter: {
        bbb.appendInvokeInstruction(
            JITRT_SetCurrentAwaiter, i.GetOperand(0), env_->asm_tstate);
        break;
      }
      case Opcode::kYieldValue: {
        auto hir_instr = static_cast<const YieldValue*>(&i);
        Instruction* instr = bbb.appendInstr(
            hir_instr->output(),
            Instruction::kYieldValue,
            env_->asm_tstate,
            hir_instr->reg());
        finishYield(bbb, instr, hir_instr);
        break;
      }
      case Opcode::kInitialYield: {
        auto hir_instr = static_cast<const InitialYield*>(&i);
        Instruction* instr = bbb.appendInstr(
            hir_instr->output(), Instruction::kYieldInitial, env_->asm_tstate);
        finishYield(bbb, instr, hir_instr);
        break;
      }
      case Opcode::kYieldAndYieldFrom:
      case Opcode::kYieldFrom:
      case Opcode::kYieldFromHandleStopAsyncIteration: {
        Instruction::Opcode op = [&] {
          if (opcode == Opcode::kYieldAndYieldFrom) {
            return Instruction::kYieldFromSkipInitialSend;
          } else if (opcode == Opcode::kYieldFrom) {
            return Instruction::kYieldFrom;
          } else {
            return Instruction::kYieldFromHandleStopAsyncIteration;
          }
        }();
        Instruction* instr = bbb.appendInstr(
            i.output(), op, env_->asm_tstate, i.GetOperand(0), i.GetOperand(1));
        finishYield(bbb, instr, static_cast<const DeoptBase*>(&i));
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
              reinterpret_cast<uint64_t>(&jit::g_iterDoneSentinel);
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
        auto cache = Runtime::get()->allocateLoadAttrCache();
        bbb.appendCallInstruction(
            dst, jit::LoadAttrCache::invoke, cache, base, name);
        break;
      }
      case Opcode::kLoadAttrSpecial: {
        auto instr = static_cast<const LoadAttrSpecial*>(&i);
        bbb.appendCallInstruction(
            instr->output(),
#if PY_VERSION_HEX >= 0x030C0000
            JITRT_LookupAttrSpecial,
#else
            Cix_special_lookup,
            env_->asm_tstate,
#endif
            instr->GetOperand(0),
            instr->id()
#if PY_VERSION_HEX >= 0x030C0000
                ,
            instr->failureFmtStr()
#endif
        );
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
        bbb.appendCallInstruction(
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
        bbb.appendCallInstruction(
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
        bbb.appendCallInstruction(dst, JITRT_GetMethod, base, name);
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
        auto cache = Runtime::get()->allocateLoadMethodCache();
        if (getConfig().collect_attr_cache_stats) {
          BorrowedRef<PyCodeObject> code = instr->frameState()->code;
          cache->initCacheStats(
              PyUnicode_AsUTF8(code->co_filename),
              PyUnicode_AsUTF8(code->co_name));
        }
        bbb.appendCallInstruction(
            dst, LoadMethodCache::lookupHelper, cache, base, name);
        break;
      }
      case Opcode::kLoadModuleAttrCached: {
        JIT_DCHECK(
            getConfig().attr_caches,
            "Inline caches must be enabled to use LoadModuleAttrCached");
        auto instr = static_cast<const LoadModuleAttrCached*>(&i);
        Instruction* name = getNameFromIdx(bbb, instr);
        auto cache = Runtime::get()->allocateLoadModuleAttrCache();
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
        auto cache_entry = Runtime::get()->allocateLoadModuleMethodCache();
        bbb.appendCallInstruction(
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
        bbb.appendCallInstruction(
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
        MakeIncref(bbb, i, false);
        break;
      }
      case Opcode::kXIncref: {
        MakeIncref(bbb, i, true);
        break;
      }
      case Opcode::kDecref: {
        MakeDecref(bbb, i, false);
        break;
      }
      case Opcode::kXDecref: {
        MakeDecref(bbb, i, true);
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
        constexpr int32_t kOffset = offsetof(
            PyThreadState,
#if PY_VERSION_HEX >= 0x030C0000
            current_exception
#else
            curexc_type
#endif
        );
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
        auto cache = cinderx::getModuleState()->cacheManager()->getGlobalCache(
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
        auto cache = Runtime::get()->allocateStoreAttrCache();
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
        uint64_t func = reinterpret_cast<uint64_t>(_PyObject_Vectorcall);
#if PY_VERSION_HEX < 0x030C0000
        flags |= (hir_instr.flags() & CallFlags::Awaited)
            ? Ci_Py_AWAITED_CALL_MARKER
            : 0;
#else
        if (!(hir_instr.func()->type() <= TFunc)) {
          // Calls to things which aren't simple Python functions will
          // need to check the eval breaker. We do this in a helper instead
          // of injecting it after every call.
          func = reinterpret_cast<uint64_t>(JITRT_Vectorcall);
        }
#endif
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kVectorCall,
            // TASK(T140174965): This should be MemImm.
            Imm{func},
            Imm{flags});
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
        auto rt_helper = (instr.flags() & CallFlags::Awaited)
            ? JITRT_CallFunctionExAwaited
            : JITRT_CallFunctionEx;
        bbb.appendCallInstruction(
            instr.output(),
            rt_helper,
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
#if PY_VERSION_HEX >= 0x030C0000
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
#else
        JIT_ABORT("CallIntrinsic is only supported in Python 3.12+");
#endif
        break;
      }
      case Opcode::kCallMethod: {
        auto& hir_instr = static_cast<const CallMethod&>(i);
        size_t flags = 0;
#if PY_VERSION_HEX < 0x030C0000
        flags |= (hir_instr.flags() & CallFlags::Awaited)
            ? Ci_Py_AWAITED_CALL_MARKER
            : 0;
#endif
        Instruction* instr = bbb.appendInstr(
            hir_instr.output(),
            Instruction::kVectorCall,
            // TASK(T140174965): This should be MemImm.
            Imm{reinterpret_cast<uint64_t>(JITRT_Call)},
            Imm{flags});
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
          void** indir = env_->rt->findFunctionEntryCache(func);
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
        auto type = instr->type();
        decltype(JITRT_SetI8_InArray)* func = nullptr;

        if (type <= TCInt8) {
          func = JITRT_SetI8_InArray;
        } else if (type <= TCUInt8) {
          func = JITRT_SetU8_InArray;
        } else if (type <= TCInt16) {
          func = JITRT_SetI16_InArray;
        } else if (type <= TCUInt16) {
          func = JITRT_SetU16_InArray;
        } else if (type <= TCInt32) {
          func = JITRT_SetI32_InArray;
        } else if (type <= TCUInt32) {
          func = JITRT_SetU32_InArray;
        } else if (type <= TCInt64) {
          func = JITRT_SetI64_InArray;
        } else if (type <= TCUInt64) {
          func = JITRT_SetU64_InArray;
        } else if (type <= TObject) {
          func = JITRT_SetObj_InArray;
        } else {
          JIT_ABORT("Unknown array type {}", type.toString());
        }

        bbb.appendInvokeInstruction(
            func, instr->ob_item(), instr->value(), instr->idx());
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
            Ci_DictOrChecked_SetItem,
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
            Ci_ListOrCheckedList_Append,
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
            instr->output(), jit::invokeIterNext, instr->GetOperand(0));
        break;
      }
      case Opcode::kLoadEvalBreaker: {
        // NB: This corresponds to an atomic load with
        // std::memory_order_relaxed. It's correct on x86-64 but probably isn't
        // on other architectures.
        hir::Register* dest = i.output();
#if PY_VERSION_HEX >= 0x030D0000
        Instruction* tstate = env_->asm_tstate;
        // tstate->ceval.eval_breaker
        static_assert(
            sizeof(reinterpret_cast<PyThreadState*>(0)->eval_breaker) == 8,
            "Eval breaker is not a 8 byte value");
        bbb.appendInstr(
            dest,
            Instruction::kMove,
            Ind{tstate, offsetof(PyThreadState, eval_breaker)});
#elif PY_VERSION_HEX >= 0x030C0000
        // eval_breaker is in the runtime, which the code is generated against,
        // load it directly.
        static_assert(
            sizeof(reinterpret_cast<PyThreadState*>(0)
                       ->interp->ceval.eval_breaker) == 4,
            "Eval breaker is not a 4 byte value");
        bbb.appendInstr(
            dest,
            Instruction::kMove,
            MemImm{reinterpret_cast<int*>(
                &ThreadedCompileContext::interpreter()->ceval.eval_breaker)});
#else
        Instruction* tstate = env_->asm_tstate;
        // tstate->interp->ceval.eval_breaker
        static_assert(
            sizeof(reinterpret_cast<PyThreadState*>(0)
                       ->interp->ceval.eval_breaker) == 4,
            "Eval breaker is not a 4 byte value");
        Instruction* interp = bbb.appendInstr(
            Instruction::kMove,
            OutVReg{OperandBase::k64bit},
            Ind{tstate, offsetof(PyThreadState, interp)});
        bbb.appendInstr(
            dest,
            Instruction::kMove,
            Ind{interp, offsetof(PyInterpreterState, ceval.eval_breaker)});
#endif
        break;
      }
      case Opcode::kRunPeriodicTasks: {
        auto helper =
#if PY_VERSION_HEX < 0x030C0000
            Cix_eval_frame_handle_pending
#else
            _Py_HandlePending
#endif
            ;
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
#if PY_VERSION_HEX < 0x030C0000 || defined(ENABLE_LIGHTWEIGHT_FRAMES)
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
        // TASK(T109706798): Support calling from generators and inlining
        // generators.
        //
        // Consider linking all shadow frame prev pointers in function prologue,
        // since they need not happen with every call -- just the data pointers
        // need to be reset with every call.
        //
        // If we manage to optimize leaf calls to a series of non-deopting
        // instructions, we could also remove BeginInlinedFunction and
        // EndInlinedFunction completely.
#endif
#if PY_VERSION_HEX < 0x030C0000
        if (kPyDebug) {
          bbb.appendInvokeInstruction(
              assertShadowCallStackConsistent, env_->asm_tstate);
        }
        Instruction* caller_shadow_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kLea,
            Stk{PhyLocation(static_cast<int32_t>(frameOffsetBefore(instr)))});
        // There is already a shadow frame for the caller function.
        Instruction* callee_shadow_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kLea,
            Stk{PhyLocation(static_cast<int32_t>(frameOffsetOf(instr)))});

        bbb.appendInstr(
            OutInd{callee_shadow_frame, SHADOW_FRAME_FIELD_OFF(prev)},
            Instruction::kMove,
            caller_shadow_frame);
        uintptr_t data = _PyShadowFrame_MakeData(rtfs, PYSF_RTFS, PYSF_JIT);
        Instruction* data_reg =
            bbb.appendInstr(OutVReg{}, Instruction::kMove, data);
        bbb.appendInstr(
            OutInd{callee_shadow_frame, SHADOW_FRAME_FIELD_OFF(data)},
            Instruction::kMove,
            data_reg);
        // Set orig_data
        // This is only necessary when in normal-frame mode because the frame
        // is already materialized on function entry. It is lazily filled when
        // the frame is materialized in shadow-frame mode.
        if (func_->frameMode == jit::FrameMode::kNormal) {
          bbb.appendInstr(
              OutInd{
                  callee_shadow_frame, JIT_SHADOW_FRAME_FIELD_OFF(orig_data)},
              Instruction::kMove,
              data_reg);
        }
        // Set our shadow frame as top of shadow stack
        bbb.appendInstr(
            OutInd{env_->asm_tstate, offsetof(PyThreadState, shadow_frame)},
            Instruction::kMove,
            callee_shadow_frame);
        if (kPyDebug) {
          bbb.appendInvokeInstruction(
              assertShadowCallStackConsistent, env_->asm_tstate);
        }
#elif defined(ENABLE_LIGHTWEIGHT_FRAMES)
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
        PyObject* func_val = cinderx::getModuleState()->frameReifier();
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
                (ssize_t)offsetof(FrameHeader, func) -
                    (ssize_t)sizeof(FrameHeader)},
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
          MakeIncref(bbb, code_reg, false);
        }

        // Set our frame as top of stack
#if PY_VERSION_HEX >= 0x030D0000
        if (!_Py_IsImmortal(func_val)) {
          MakeIncref(bbb, func_reg, false);
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
#if PY_VERSION_HEX < 0x030C0000
        // TASK(T109706798): Support calling from generators and inlining
        // generators.
        if (kPyDebug) {
          bbb.appendInvokeInstruction(
              assertShadowCallStackConsistent, env_->asm_tstate);
        }
        // callee_shadow_frame <- tstate.shadow_frame
        Instruction* callee_shadow_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{env_->asm_tstate, offsetof(PyThreadState, shadow_frame)});

        // Check if the callee has been materialized into a PyFrame. Use the
        // flags below.
        static_assert(
            PYSF_PYFRAME == 1 && _PyShadowFrame_NumPtrKindBits == 2,
            "Unexpected constants");
        Instruction* shadow_frame_data = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{callee_shadow_frame, SHADOW_FRAME_FIELD_OFF(data)});
        bbb.appendInstr(Instruction::kBitTest, shadow_frame_data, Imm{0});

        // caller_shadow_frame <- callee_shadow_frame.prev
        Instruction* caller_shadow_frame = bbb.appendInstr(
            OutVReg{},
            Instruction::kMove,
            Ind{callee_shadow_frame, SHADOW_FRAME_FIELD_OFF(prev)});
        // caller_shadow_frame -> tstate.shadow_frame
        bbb.appendInstr(
            OutInd{env_->asm_tstate, offsetof(PyThreadState, shadow_frame)},
            Instruction::kMove,
            caller_shadow_frame);
        // Unlink PyFrame if needed. Someone might have materialized all of the
        // PyFrames via PyEval_GetFrame or similar.
        auto done_block = bbb.allocateBlock();
        bbb.appendBranch(Instruction::kBranchNC, done_block);
        // TASK(T109445584): Remove this unused block.
        bbb.appendBlock(bbb.allocateBlock());
        // We already unlinked the frame up above, this just needs to release
        // the reified frame.
        bbb.appendInvokeInstruction(JITRT_UnlinkPyFrame, env_->asm_tstate);
        bbb.appendBlock(done_block);
        if (kPyDebug) {
          bbb.appendInvokeInstruction(
              assertShadowCallStackConsistent, env_->asm_tstate);
        }
#elif defined(ENABLE_LIGHTWEIGHT_FRAMES)
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
                (ssize_t)offsetof(FrameHeader, func) -
                    (ssize_t)sizeof(FrameHeader)});
        JIT_DCHECK(
            JIT_FRAME_INITIALIZED == 2,
            "JIT_FRAME_INITIALIZED changed"); // this is the bit we're testing
                                              // below
        bbb.appendInstr(Instruction::kBitTest, rtfs_reg, Imm{1});
        auto done_block = bbb.allocateBlock();
        auto not_materialized_block = bbb.allocateBlock();
        bbb.appendBranch(Instruction::kBranchNC, not_materialized_block);
        bbb.appendBlock(bbb.allocateBlock());

        // The frame was materialized, let's use the unlink helper to clean
        // things up.
        bbb.appendInvokeInstruction(JITRT_UnlinkFrame, false);
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
        MakeDecref(
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
          MakeDecref(
              bbb,
              func_reg,
              std::optional<destructor>(PyFunction_Type.tp_dealloc));
        }
#endif
#else
        if (!_Py_IsImmortal(code.get())) {
          Instruction* code_reg =
              bbb.appendInstr(OutVReg{}, Instruction::kMove, code.get());
          MakeDecref(
              bbb, code_reg, std::optional<destructor>(PyCode_Type.tp_dealloc));
        }
#endif

        bbb.appendBlock(done_block);
#endif
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
#if PY_VERSION_HEX >= 0x030E0000
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
#elif PY_VERSION_HEX >= 0x030C0000 && ENABLE_LAZY_IMPORTS
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
        // the callable is always null, and all the components to be
        // concatenated will be in the args argument.

        Instruction* lir = bbb.appendInstr(
            instr.output(),
            Instruction::kVectorCall,
            JITRT_BuildString,
            nullptr,
            nullptr);
        for (size_t operandIdx = 0; operandIdx < instr.NumOperands();
             operandIdx++) {
          lir->addOperands(VReg{bbb.getDefInstr(instr.GetOperand(operandIdx))});
        }
        lir->addOperands(Imm{0});

        break;
      }
      case Opcode::kWaitHandleLoadWaiter: {
#if PY_VERSION_HEX < 0x030C0000
        const auto& instr = static_cast<const WaitHandleLoadWaiter&>(i);
        Instruction* base = bbb.getDefInstr(instr.reg());
        int32_t offset = offsetof(Ci_PyWaitHandleObject, wh_waiter);
        bbb.appendInstr(instr.output(), Instruction::kMove, Ind{base, offset});
#endif
        break;
      }
      case Opcode::kWaitHandleLoadCoroOrResult: {
#if PY_VERSION_HEX < 0x030C0000
        const auto& instr = static_cast<const WaitHandleLoadCoroOrResult&>(i);
        Instruction* base = bbb.getDefInstr(instr.reg());
        int32_t offset = offsetof(Ci_PyWaitHandleObject, wh_coro_or_result);
        bbb.appendInstr(instr.output(), Instruction::kMove, Ind{base, offset});
#endif
        break;
      }
      case Opcode::kWaitHandleRelease: {
#if PY_VERSION_HEX < 0x030C0000
        const auto& instr = static_cast<const WaitHandleRelease&>(i);
        bbb.appendInstr(
            OutInd{
                bbb.getDefInstr(instr.reg()),
                static_cast<int32_t>(
                    offsetof(Ci_PyWaitHandleObject, wh_coro_or_result))},
            Instruction::kMove,
            0);
        bbb.appendInstr(
            OutInd{
                bbb.getDefInstr(instr.reg()),
                static_cast<int32_t>(
                    offsetof(Ci_PyWaitHandleObject, wh_waiter))},
            Instruction::kMove,
            0);
#endif
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
#if PY_VERSION_HEX >= 0x030C0000
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
#elif PY_VERSION_HEX >= 0x030C0000

        bbb.appendInstr(
            OutInd{frame, offsetof(_PyInterpreterFrame, prev_instr)},
            Instruction::kMove,
            prev_instr_ptr);
#else
#endif
#endif
        break;
      }
      case Opcode::kSend: {
        auto& hir_instr = static_cast<const Send&>(i);
        // Note: asm_interpreter_frame isn't right for inlined functions, but we
        // never inline generators so this is fine for now.
        bbb.appendInstr(
            hir_instr.output(),
            Instruction::kCall,
            Imm{reinterpret_cast<uint64_t>(JITRT_GenSend)},
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
        bbb.appendCallInstruction(
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

} // namespace jit::lir
