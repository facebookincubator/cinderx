// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/deopt.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/hir/printer.h"

#ifdef ENABLE_USDT
#include <usdt/usdt.h>
#else
#define USDT(...)
#endif

#include <shared_mutex>

namespace jit {

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

} // namespace

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
  return reinterpret_cast<PyObject*>(readRaw(value));
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
    case jit::hir::ValueKind::kObject:
      return Ref<>::create(reinterpret_cast<PyObject*>(raw));
  }
  JIT_ABORT("Unhandled ValueKind");
}

static void reifyLocalsplus(
    CiPyFrameObjType* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const MemoryView& mem) {
#if PY_VERSION_HEX < 0x030C0000
  PyObject** localsplus = &frame->f_localsplus[0];
#else
  Ci_STACK_TYPE* localsplus = &frame->localsplus[0];
#endif

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

static void reifyStack(
    CiPyFrameObjType* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const MemoryView& mem) {
#if PY_VERSION_HEX >= 0x030E0000
  frame->stackpointer =
      &frame->localsplus
           [_PyFrame_GetCode(frame)->co_nlocalsplus + frame_meta.stack.size()];
  _PyStackRef* stack_top = frame->stackpointer - 1;
#elif PY_VERSION_HEX < 0x030C0000
  frame->f_stackdepth = frame_meta.stack.size();
  PyObject** stack_top = &frame->f_valuestack[frame->f_stackdepth - 1];
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

#if PY_VERSION_HEX < 0x030E0000
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
static BCIndex getDeoptResumeIndex(
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame,
    bool forced_deopt) {
  // We only need to consider guards as the deopt cause in the inner-most
  // inlined location. If we are reifying the conceptual frames for an inlined
  // function's callers then these will be resumed by the interpreter in
  // future and will never be a JIT guard failure.
  bool is_innermost = &frame == &meta.innermostFrame();
  if ((is_innermost &&
       (meta.reason == DeoptReason::kGuardFailure ||
        meta.reason == DeoptReason::kRaise)) ||
      forced_deopt) {
    return frame.cause_instr_idx;
  }
  return BytecodeInstruction(frame.code, frame.cause_instr_idx)
      .nextInstrOffset();
}
#endif

#if PY_VERSION_HEX < 0x030C0000

static void reifyBlockStack(
    PyFrameObject* frame,
    const jit::hir::BlockStack& block_stack) {
  std::size_t bs_size = block_stack.size();
  frame->f_iblock = bs_size;
  for (std::size_t i = 0; i < bs_size; i++) {
    const auto& block = block_stack.at(i);
    frame->f_blockstack[i].b_type = block.opcode;
    frame->f_blockstack[i].b_handler = block.handler_off.asIndex().value();
    frame->f_blockstack[i].b_level = block.stack_level;
  }
}

static void reifyFrameImpl(
    PyFrameObject* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    bool forced_deopt,
    const uint64_t* regs) {
  frame->f_locals = nullptr;
  frame->f_trace = nullptr;
  frame->f_trace_opcodes = 0;
  frame->f_trace_lines = 1;

  // If we're forcing a deopt leave the frame state as-is.
  if (!forced_deopt) {
    frame->f_state = meta.reason == DeoptReason::kGuardFailure
        ? FRAME_EXECUTING
        : FRAME_UNWINDING;
  }

  // Instruction pointer.
  frame->f_lasti =
      getDeoptResumeIndex(meta, frame_meta, forced_deopt).value() - 1;

  // Saturate at -1 as some things specifically check for this to see if a
  // frame is just created but not run yet.
  frame->f_lasti = std::max(frame->f_lasti, -1);

  MemoryView mem{regs};
  reifyLocalsplus(frame, meta, frame_meta, mem);
  reifyStack(frame, meta, frame_meta, mem);
  reifyBlockStack(frame, frame_meta.block_stack);
  // Generator/frame linkage happens in `materializePyFrame` in frame.cpp
}

#else // PY_VERSION_HEX < 0x030C0000

bool shouldResumeInterpreterInErrorHandler(DeoptReason reason) {
  switch (reason) {
    case DeoptReason::kGuardFailure:
    case DeoptReason::kRaise:
      return false;
    case jit::DeoptReason::kYieldFrom:
    case jit::DeoptReason::kUnhandledException:
    case jit::DeoptReason::kUnhandledUnboundLocal:
    case jit::DeoptReason::kUnhandledUnboundFreevar:
    case jit::DeoptReason::kUnhandledNullField:
    case jit::DeoptReason::kRaiseStatic:
      return true;
  }
}

static void reifyFrameImpl(
    _PyInterpreterFrame* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    bool forced_deopt,
    const uint64_t* regs) {
#if PY_VERSION_HEX >= 0x030E0000
  BorrowedRef<PyCodeObject> code_obj = frameCode(frame);
  int cause_instr_idx = frame_meta.cause_instr_idx.value();
  // Resume with instr_ptr pointing to the cause instruction if we are entering
  // the interpreter to re-run a failed instruction, or implement an instruction
  // we don't JIT.
  frame->instr_ptr = _PyCode_CODE(code_obj) + cause_instr_idx;
  if (&frame_meta != &meta.innermostFrame()) {
    // If we're not the inner most frame then we're always deopting
    // after the instruction that executed
    frame->instr_ptr += inlineCacheSize(code_obj, cause_instr_idx) + 1;
  } else if (shouldResumeInterpreterInErrorHandler(meta.reason)) {
    // Otherwise, have instr_ptr point to the next instruction
    // (minus one _Py_CODEUNIT for some reason).
    frame->instr_ptr += inlineCacheSize(code_obj, cause_instr_idx);
  }
#else
  // Note frame->prev_instr doesn't point to the previous instruction, it
  // actually points to the memory location sizeof(Py_CODEUNIT) bytes before
  // the next instruction to execute. This means it might point to inline-
  // cache data or a negative location.
  int prev_idx =
      (getDeoptResumeIndex(meta, frame_meta, forced_deopt) - 1).value();
  frame->prev_instr = _PyCode_CODE(_PyFrame_GetCode(frame)) + prev_idx;
#endif

  MemoryView mem{regs};
  reifyLocalsplus(frame, meta, frame_meta, mem);
  reifyStack(frame, meta, frame_meta, mem);
}

#endif // PY_VERSION_HEX >= 0x030C0000

void reifyFrame(
    CiPyFrameObjType* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const uint64_t* regs) {
  reifyFrameImpl(frame, meta, frame_meta, false /* forced_deopt */, regs);
}

void reifyGeneratorFrame(
    CiPyFrameObjType* frame,
    const DeoptMetadata& meta,
    const DeoptFrameMetadata& frame_meta,
    const void* base) {
  uint64_t regs[codegen::NUM_GP_REGS]{};
  regs[codegen::arch::reg_frame_pointer_loc.loc] =
      reinterpret_cast<uint64_t>(base);
  constexpr bool force_deopt = PY_VERSION_HEX >= 0x030C0000 ? false : true;
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

static DeoptReason getDeoptReason(const jit::hir::DeoptBase& instr) {
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
    case jit::hir::Opcode::kYieldAndYieldFrom:
    case jit::hir::Opcode::kYieldFromHandleStopAsyncIteration:
    case jit::hir::Opcode::kYieldFrom: {
      return DeoptReason::kYieldFrom;
    }
    case jit::hir::Opcode::kRaise: {
      return DeoptReason::kRaise;
    }
    case jit::hir::Opcode::kRaiseStatic: {
      return DeoptReason::kRaiseStatic;
    }
    default: {
      return DeoptReason::kUnhandledException;
    }
  }
}

DeoptMetadata DeoptMetadata::fromInstr(const jit::hir::DeoptBase& instr) {
  auto get_source = [&](jit::hir::Register* reg) {
    reg = hir::modelReg(reg);
    auto instr = reg->instr();
    if (isAnyLoadMethod(*instr)) {
      return LiveValue::Source::kLoadMethod;
    }
    return LiveValue::Source::kUnknown;
  };

  DeoptMetadata meta;
  meta.live_values.resize(instr.live_regs().size());

  std::unordered_map<jit::hir::Register*, int> reg_idx;
  int i = 0;
  for (const auto& reg_state : instr.live_regs()) {
    auto reg = reg_state.reg;

    LiveValue lv = {
        // location will be filled in once we've generated code
        .location = 0,
        .ref_kind = reg_state.ref_kind,
        .value_kind = reg_state.value_kind,
        .source = get_source(reg),
    };
    meta.live_values[i] = std::move(lv);
    reg_idx[reg] = i;
    i++;
  }

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
        meta.localsplus.resize(nlocalsplus);
        for (size_t j = 0; j < nlocalsplus; ++j) {
          meta.localsplus[j] = get_reg_idx(fs->localsplus[j]);
        }
      };

  auto populate_stack = [get_reg_idx](
                            DeoptFrameMetadata& meta, hir::FrameState* fs) {
    std::unordered_set<jit::hir::Register*> lms_on_stack;
    meta.stack.resize(fs->stack.size());
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
  meta.frame_meta.resize(num_frames + 1); // +1 for caller

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

} // namespace jit
