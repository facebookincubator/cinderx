// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/deopt.h"

// clang-tidy off
#include "cinderx/UpstreamBorrow/borrowed.h"
// clang-tidy on

#include "internal/pycore_ceval.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/hir/printer.h"

#ifdef ENABLE_USDT
#include <usdt/usdt.h>
#else
#define USDT(...)
#endif

#include <shared_mutex>

namespace cinderx::jit {

namespace {

// Set of interned strings for deopt descriptions.
std::unordered_set<std::string> s_descrs;
std::shared_mutex s_descrs_mutex;

// Intern a description string and return a reference to it.
const std::string& internDescr(const std::string& descr) {
  std::shared_lock reader_guard{s_descrs_mutex};
  if (auto iter = s_descrs.find(descr); iter != s_descrs.end()) {
    return *iter;
  }
  reader_guard.unlock();
  std::unique_lock writer_guard{s_descrs_mutex};
  return *s_descrs.emplace(descr).first;
}

std::unordered_set<hir::Register*> collectFrameStateRegs(hir::FrameState* fs) {
  std::unordered_set<hir::Register*> regs;
  for (; fs != nullptr; fs = fs->parent) {
    for (hir::Register* local : fs->localsplus) {
      if (local != nullptr) {
        regs.insert(local);
      }
    }
    for (hir::Register* stack : fs->stack) {
      if (stack != nullptr) {
        regs.insert(stack);
      }
    }
  }
  return regs;
}

void reifyLocalsplus(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const MemoryView& mem) {
  Ci_STACK_TYPE* localsplus = &frame->localsplus[0];

  BorrowedRef<PyCodeObject> code = frameCode(frame);
  int free_offset = numLocalsplus(code) - numFreevars(code);
  // Local variables are not initialized in the frame
  for (std::size_t i = 0; i < free_offset && i < frame_meta.localsplus.size();
       i++) {
    const LiveValue* value = meta.getLocalValue(i, frame_meta);
    if (value == nullptr) {
      // Value is dead
      *localsplus = Ci_STACK_NULL;
    } else {
      PyObject* obj = mem.readOwned(*value).release();
      *localsplus = Ci_STACK_STEAL(obj);
    }
    localsplus++;
  }

  // Free variables are initialized
  for (std::size_t i = free_offset; i < frame_meta.localsplus.size(); i++) {
    const LiveValue* value = meta.getLocalValue(i, frame_meta);
    if (value == nullptr) {
      Ci_STACK_CLEAR(*localsplus);
    } else {
      PyObject* obj = mem.readOwned(*value).release();
      Ci_STACK_XSETREF(*localsplus, obj);
    }
    localsplus++;
  }
}

void reifyStack(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const MemoryView& mem) {
#if PY_VERSION_HEX >= 0x030E0000
  frame->stackpointer =
      &frame->localsplus
           [_PyFrame_GetCode(frame)->co_nlocalsplus + frame_meta.stack.size()];
  _PyStackRef* stack_top = frame->stackpointer - 1;
#else
  frame->stacktop =
      _PyFrame_GetCode(frame)->co_nlocalsplus + frame_meta.stack.size();
  PyObject** stack_top = &frame->localsplus[frame->stacktop - 1];
#endif
  for (int i = frame_meta.stack.size() - 1; i >= 0; i--) {
    const auto& value = meta.getStackValue(i, frame_meta);
    Ref<> obj = mem.readOwned(value);

    // When we are deoptimizing a JIT-compiled function that contains an
    // optimizable LoadMethod, we need to be able to know whether or not the
    // LoadMethod returned a bound method object in order to properly
    // reconstruct the stack for the interpreter. We use Py_None as the
    // LoadMethodResult to indicate that it was a non-method like object, which
    // we need to replace with nullptr to match the interpreter semantics.
    if (value.isLoadMethodResult() && obj == Py_None) {
      *stack_top = Ci_STACK_NULL;
    } else {
      *stack_top = Ci_STACK_STEAL(obj.release());
    }
    stack_top--;
  }
}

// This function handles all computation of the index to resume at for a given
// deopt.
//
// The first thing is it considers is whether the deopt is a guard failure or
// due to an exception being raised as part of the execution. Guard failures
// mean the JITed opcode has failed and needs to be re-run in the interpreter,
// exceptions mean the opcode has succeeded but there has been an exceptional
// condition so we want to resume the *next* opcode in the interpreter.
//
// The second thing it handles is forced deopt. If we're forcing a deopt due
// to no actual guard failure or exception, then we want to resume at the
// instruction we're currently stopped on.
BCIndex getDeoptResumeIndex(
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame,
    bool forced_deopt,
    bool is_instrumentation_deopt = false) {
  // We only need to consider guards as the deopt cause in the inner-most
  // inlined location. If we are reifying the conceptual frames for an inlined
  // function's callers then these will be resumed by the interpreter in
  // future and will never be a JIT guard failure.
  bool is_innermost = &frame == &meta.innermostFrame();

  // For instrumentation deopts: kPeriodicTaskFailure means the bytecode
  // hasn't completed (RunPeriodicTasks) — re-execute. Other reasons mean
  // the bytecode's C call completed — advance past it.
  if (is_instrumentation_deopt && is_innermost) {
    if (meta.reason == DeoptReason::kPeriodicTaskFailure) {
      return frame.cause_instr_idx;
    }
    return BytecodeInstruction(frame.code, frame.cause_instr_idx)
        .nextInstrOffset();
  }

  if ((is_innermost &&
       (meta.reason == DeoptReason::kGuardFailure ||
        meta.reason == DeoptReason::kRaise)) ||
      forced_deopt) {
    return frame.cause_instr_idx;
  }

#if PY_VERSION_HEX >= 0x030E0000
  if (PyErr_Occurred() && is_innermost) {
    // On 3.14+ the traceback is going to be generated based on instr_ptr
    // and then we'll dispatch to the error handler. We're never going to
    // execute the instruction but we need the instr_ptr to point at the
    // faulting instruction for stack traces to show up correctly.
    return BytecodeInstruction(frame.code, frame.cause_instr_idx)
               .nextInstrOffset()
               .asIndex() -
        1;
  }
#endif
  return BytecodeInstruction(frame.code, frame.cause_instr_idx)
      .nextInstrOffset();
}

void reifyFrameImpl(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    bool forced_deopt,
    const uint64_t* regs,
    bool is_instrumentation_deopt = false) {
  // Note frame->prev_instr doesn't point to the previous instruction, it
  // actually points to the memory location sizeof(Py_CODEUNIT) bytes before
  // the next instruction to execute. This means it might point to inline-
  // cache data or a negative location.
  //
  // For instrumentation deopts, getDeoptResumeIndex re-executes for
  // kPeriodicTaskFailure and advances for all other reasons.
  int prev_idx =
      (getDeoptResumeIndex(
           meta, frame_meta, forced_deopt, is_instrumentation_deopt) -
       1)
          .value();

#if PY_VERSION_HEX >= 0x030E0000
#ifdef Py_GIL_DISABLED
  PyThreadState* tstate = _PyThreadState_GET();
  frame->instr_ptr =
      _PyEval_GetExecutableCode(tstate, _PyFrame_GetCode(frame)) + prev_idx + 1;
  frame->tlbc_index = reinterpret_cast<_PyThreadStateImpl*>(tstate)->tlbc_index;
#else
  frame->instr_ptr = _PyCode_CODE(_PyFrame_GetCode(frame)) + prev_idx + 1;
#endif
#else
  frame->prev_instr = _PyCode_CODE(_PyFrame_GetCode(frame)) + prev_idx;
#endif

  MemoryView mem{regs};
  reifyLocalsplus(frame, meta, frame_meta, mem);
  reifyStack(frame, meta, frame_meta, mem);
}

DeoptReason getDeoptReason(const jit::hir::DeoptBase& instr) {
  switch (instr.opcode()) {
    case jit::hir::Opcode::kCheckVar: {
      return DeoptReason::kUnhandledUnboundLocal;
    }
    case jit::hir::Opcode::kCheckFreevar: {
      return DeoptReason::kUnhandledUnboundFreevar;
    }
    case jit::hir::Opcode::kCheckField: {
      return DeoptReason::kUnhandledNullField;
    }
    case jit::hir::Opcode::kDeopt:
    case jit::hir::Opcode::kDeoptPatchpoint:
    case jit::hir::Opcode::kGuard:
    case jit::hir::Opcode::kGuardIs:
    case jit::hir::Opcode::kGuardType:
    case jit::hir::Opcode::kLoadSplitDictItem: {
      return DeoptReason::kGuardFailure;
    }
    case jit::hir::Opcode::kRaise: {
      return DeoptReason::kRaise;
    }
    case jit::hir::Opcode::kRaiseStatic: {
      return DeoptReason::kRaiseStatic;
    }
    case jit::hir::Opcode::kRunPeriodicTasks: {
      return DeoptReason::kPeriodicTaskFailure;
    }
    default: {
      return DeoptReason::kUnhandledException;
    }
  }
}

} // namespace

DeoptLiveRegFilter::DeoptLiveRegFilter(const hir::DeoptBase& instr)
    : instr_(instr),
      frame_state_regs_(collectFrameStateRegs(instr.frameState())) {}

bool DeoptLiveRegFilter::isUsed(const hir::RegState& reg_state) const {
  if (reg_state.ref_kind == hir::RefKind::kOwned) {
    return true;
  }

  hir::Register* reg = reg_state.reg;
  if (instr_.guiltyReg() == reg) {
    return true;
  }

  return frame_state_regs_.contains(reg);
}

hir::ValueKind deoptValueKind(hir::Type type) {
  if (type <= jit::hir::TCBool) {
    return jit::hir::ValueKind::kBool;
  }

  if (type <= jit::hir::TCDouble) {
    return jit::hir::ValueKind::kDouble;
  }

  // The type predicates here are gross and indicate a deeper problem with how
  // we're using Types earlier in the pipeline: we use `LoadNull` to
  // zero-initialize locals with primitive types (currently done in SSAify). It
  // works fine at runtime and a proper fix likely involves reworking HIR's
  // support for constant values, so we paper over the issue here for the
  // moment.
  if (type.couldBe(jit::hir::TCUnsigned | jit::hir::TCSigned)) {
    if (type <= (jit::hir::TCUnsigned | jit::hir::TNullptr)) {
      return jit::hir::ValueKind::kUnsigned;
    }
    if (type <= (jit::hir::TCSigned | jit::hir::TNullptr)) {
      return jit::hir::ValueKind::kSigned;
    }
  } else if (type.couldBe(jit::hir::TCDouble)) {
    return jit::hir::ValueKind::kDouble;
  }

  JIT_CHECK(
      type <= jit::hir::TOptObject, "Unexpected type {} in deopt value", type);
  return jit::hir::ValueKind::kObject;
}

const char* deoptReasonName(DeoptReason reason) {
  switch (reason) {
#define REASON(name)         \
  case DeoptReason::k##name: \
    return #name;
    DEOPT_REASONS(REASON)
#undef REASON
  }
  JIT_ABORT("Invalid DeoptReason {}", static_cast<int>(reason));
}

BorrowedRef<> MemoryView::readBorrowed(const LiveValue& value) const {
  JIT_CHECK(
      value.value_kind == jit::hir::ValueKind::kObject,
      "cannot materialize a borrowed primitive value");
  uint64_t raw = readRaw(value);
  if constexpr (kFreeThreadedBuild) {
    raw = stripDeferredRcTag(raw);
  }
  return reinterpret_cast<PyObject*>(raw);
}

Ref<> MemoryView::readOwned(const LiveValue& value) const {
  uint64_t raw = readRaw(value);

  switch (value.value_kind) {
    case jit::hir::ValueKind::kSigned: {
      Py_ssize_t raw_signed = bit_cast<Py_ssize_t, uint64_t>(raw);
      return Ref<>::steal(PyLong_FromSsize_t(raw_signed));
    }
    case jit::hir::ValueKind::kUnsigned:
      return Ref<>::steal(PyLong_FromSize_t(raw));
    case hir::ValueKind::kDouble:
      return Ref<>::steal(PyFloat_FromDouble(raw));
    case jit::hir::ValueKind::kBool:
      return Ref<>::create(raw ? Py_True : Py_False);
    case jit::hir::ValueKind::kObject: {
      if constexpr (kFreeThreadedBuild) {
        raw = stripDeferredRcTag(raw);
      }
      return Ref<>::create(reinterpret_cast<PyObject*>(raw));
    }
  }
  JIT_ABORT("Unhandled ValueKind");
}

Ref<> profileDeopt(const DeoptMetadata& meta, const MemoryView& mem) {
  BorrowedRef<PyCodeObject> code = meta.innermostFrame().code;
  BCOffset bc_off = meta.innermostFrame().cause_instr_idx;

  // Bytecode offset will be negative if the interpreter wants to resume
  // executing at the start of the function.  Report a negative/invalid opcode
  // for that case.
  [[maybe_unused]] int opcode = -1;
  if (bc_off.value() >= 0) {
    BytecodeInstruction bc_instr{code, bc_off};
    opcode = bc_instr.opcode();
  }

  USDT(
      python,
      deopt,
      deoptReasonName(meta.reason),
      codeQualname(code).c_str(),
      bc_off.value(),
      opcode);

  const LiveValue* live_val = meta.getGuiltyValue();
  return live_val == nullptr ? nullptr : mem.readOwned(*live_val);
}

void reifyFrame(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const uint64_t* regs,
    [[maybe_unused]] bool is_instrumentation_deopt) {
  reifyFrameImpl(
      frame,
      meta,
      frame_meta,
      false /* forced_deopt */,
      regs,
      is_instrumentation_deopt);
}

void reifyGeneratorFrame(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const void* base) {
  uint64_t regs[codegen::NUM_GP_REGS]{};
  regs[codegen::arch::reg_frame_pointer_loc.loc] =
      reinterpret_cast<uint64_t>(base);
  constexpr bool force_deopt = false;
  reifyFrameImpl(frame, meta, frame_meta, force_deopt, regs);
}

void releaseRefs(const DeoptMetadata& meta, const MemoryView& mem) {
  for (const auto& value : meta.live_values) {
    switch (value.ref_kind) {
      case jit::hir::RefKind::kUncounted:
      case jit::hir::RefKind::kBorrowed: {
        continue;
      }
      case jit::hir::RefKind::kOwned: {
        if constexpr (kFreeThreadedBuild) {
          // If the raw value has the deferred-RC tag, the matching
          // incref was skipped at function entry, so skip the decref too.
          if (value.value_kind == hir::ValueKind::kObject &&
              isDeferredRcTagged(mem.readRaw(value))) {
            break;
          }
        }
        // Read as borrowed then steal to decref.
        Ref<>::steal(mem.readBorrowed(value));
        break;
      }
    }
  }
}

void releaseRefs(const DeoptMetadata& meta, const void* base) {
  uint64_t regs[codegen::NUM_GP_REGS]{};
  regs[codegen::arch::reg_frame_pointer_loc.loc] =
      reinterpret_cast<uint64_t>(base);
  releaseRefs(meta, MemoryView{regs});
}

LiveValue::Source getLiveValueSource(jit::hir::Register* reg) {
  reg = hir::modelReg(reg);
  auto instr = reg->instr();
  if (isAnyLoadMethod(*instr)) {
    return LiveValue::Source::kLoadMethod;
  }
  return LiveValue::Source::kUnknown;
}

std::vector<const hir::RegState*> usedLiveRegs(
    const hir::LiveValuesBase& instr) {
  std::vector<const hir::RegState*> live_regs;
  live_regs.reserve(instr.live_regs().size());
  for (const auto& reg_state : instr.live_regs()) {
    live_regs.push_back(&reg_state);
  }
  return live_regs;
}

std::vector<const hir::RegState*> usedLiveRegs(const hir::DeoptBase& instr) {
  DeoptLiveRegFilter live_reg_filter{instr};
  std::vector<const hir::RegState*> live_regs;
  live_regs.reserve(instr.live_regs().size());
  for (const auto& reg_state : instr.live_regs()) {
    if (live_reg_filter.isUsed(reg_state)) {
      live_regs.push_back(&reg_state);
    }
  }
  return live_regs;
}

template <typename InstrT>
DeoptMetadata initDeoptMetadata(
    const InstrT& instr,
    std::unordered_map<jit::hir::Register*, int>& reg_idx) {
  DeoptMetadata meta;
  std::vector<const hir::RegState*> live_regs = usedLiveRegs(instr);
  meta.live_values.initialize(live_regs.size());

  int i = 0;
  for (const hir::RegState* reg_state : live_regs) {
    auto reg = reg_state->reg;

    LiveValue lv = {
        // location will be filled in once we've generated code
        .location = 0,
        .ref_kind = reg_state->ref_kind,
        .value_kind = reg_state->value_kind,
        .source = getLiveValueSource(reg),
    };
    meta.live_values[i] = std::move(lv);
    reg_idx[reg] = i;
    i++;
  }

  return meta;
}

DeoptMetadata DeoptMetadata::fromInstr(const jit::hir::DeoptBase& instr) {
  std::unordered_map<jit::hir::Register*, int> reg_idx;
  DeoptMetadata meta = initDeoptMetadata(instr, reg_idx);

  auto get_reg_idx = [&reg_idx](jit::hir::Register* reg) {
    if (reg == nullptr) {
      return -1;
    }
    auto it = reg_idx.find(reg);
    JIT_CHECK(it != reg_idx.end(), "register {} not live", reg->name());
    return it->second;
  };

  auto populate_localsplus =
      [get_reg_idx](DeoptFrameMetadata& meta, hir::FrameState* fs) {
        size_t nlocalsplus = fs->localsplus.size();
        meta.localsplus.initialize(nlocalsplus);
        for (size_t j = 0; j < nlocalsplus; ++j) {
          meta.localsplus[j] = get_reg_idx(fs->localsplus[j]);
        }
      };

  auto populate_stack = [get_reg_idx](
                            DeoptFrameMetadata& meta, hir::FrameState* fs) {
    std::unordered_set<jit::hir::Register*> lms_on_stack;
    meta.stack.initialize(fs->stack.size());
    int stack_idx = 0;

    for (auto& reg : fs->stack) {
      if (isAnyLoadMethod(*reg->instr())) {
        // Our logic for reconstructing the Python stack assumes that if a
        // value on the stack was produced by a LoadMethod instruction, it
        // corresponds to the output of a LOAD_METHOD opcode and will
        // eventually be consumed by a CALL_METHOD. That doesn't technically
        // have to be true, but it's our contention that the CPython
        // compiler will never produce bytecode that would contradict this.
        auto result = lms_on_stack.emplace(reg);
        JIT_CHECK(
            result.second,
            "load method results may only appear in one stack slot");
      }
      meta.stack[stack_idx++] = get_reg_idx(reg);
    }
  };

  auto fs = instr.frameState();
  JIT_DCHECK(
      fs != nullptr, "need FrameState to calculate inline depth of {}", instr);

  int num_frames = fs->inlineDepth();
  meta.frame_meta.initialize(num_frames + 1); // +1 for caller

  for (hir::FrameState* frame = fs; frame != nullptr; frame = frame->parent) {
    int frame_idx = num_frames--;
    // Translate locals and cells
    populate_localsplus(meta.frame_meta.at(frame_idx), frame);
    populate_stack(meta.frame_meta.at(frame_idx), frame);
    meta.frame_meta.at(frame_idx).block_stack = frame->block_stack;
    meta.frame_meta.at(frame_idx).cause_instr_idx = frame->cur_instr_offs;
    meta.frame_meta.at(frame_idx).code = frame->code.get();
  }

  if (hir::Register* guilty_reg = instr.guiltyReg()) {
    meta.guilty_value = get_reg_idx(guilty_reg);
  }

  meta.nonce = instr.nonce();
  meta.reason = getDeoptReason(instr);
  JIT_CHECK(
      meta.reason != DeoptReason::kUnhandledNullField ||
          meta.guilty_value != -1,
      "Guilty value is required for UnhandledNullField deopts");
  if (auto check = dynamic_cast<const hir::CheckBaseWithName*>(&instr)) {
    meta.eh_name = check->name();
  }

  std::string descr = instr.descr();
  if (descr.empty()) {
    descr = instr.opname();
  }

  // This is safe to do because `s_descrs` has process lifetime.
  const std::string& interned = internDescr(descr);
  meta.descr = interned.c_str();

  return meta;
}

DeoptMetadata DeoptMetadata::fromInstr(const jit::hir::LiveValuesBase& instr) {
  std::unordered_map<jit::hir::Register*, int> reg_idx;
  DeoptMetadata meta = initDeoptMetadata(instr, reg_idx);
  meta.frame_meta.initialize(1);

  meta.reason = DeoptReason::kUnhandledException;
  const std::string& interned = internDescr(std::string{instr.opname()});
  meta.descr = interned.c_str();
  return meta;
}

void visitLiveDeferredRefs(
    [[maybe_unused]] const DeoptMetadata& meta,
    [[maybe_unused]] uintptr_t frame_base,
    [[maybe_unused]] gcvisitobjects_t visit) {
#ifdef Py_GIL_DISABLED
  uint64_t regs[codegen::NUM_GP_REGS]{};
  regs[codegen::arch::reg_frame_pointer_loc.loc] = frame_base;
  MemoryView mem{regs};

  auto visit_live_value = [&](const LiveValue& live_value) {
    if (live_value.ref_kind != hir::RefKind::kOwned ||
        live_value.value_kind != hir::ValueKind::kObject) {
      return;
    }
    JIT_CHECK(
        live_value.location.loc != codegen::PhyLocation::REG_INVALID,
        "Owned live object has no physical location");
    uint64_t raw = mem.readRaw(live_value);
    if (!isDeferredRcTagged(raw)) {
      return;
    }
    PyObject* obj = reinterpret_cast<PyObject*>(stripDeferredRcTag(raw));
    (void)visit(obj, nullptr);
  };

  for (const LiveValue& live_value : meta.live_values) {
    visit_live_value(live_value);
  }
#else
  JIT_ABORT("Only for FT builds.");
#endif
}

} // namespace cinderx::jit
