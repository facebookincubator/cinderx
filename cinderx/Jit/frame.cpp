// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame.h"

#include "internal/pycore_frame.h"
#ifdef Py_GIL_DISABLED
#include "internal/pycore_ceval.h"
#include "internal/pycore_interp_structs.h"
#include "internal/pycore_pystate.h"
#endif

#include "cinderx/Common/code.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/gen_data_footer.h"
#if defined(CINDER_X86_64)
#include "cinderx/Jit/symbolizer.h"
#endif
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <optional>

namespace cinderx::jit {

namespace {

struct FrameAndLoc {
  FrameAndLoc(_PyInterpreterFrame* sf, const CodeObjLoc& l)
      : frame(sf), loc(l) {}
  _PyInterpreterFrame* frame;
  CodeObjLoc loc;
};

using UnitState = std::vector<FrameAndLoc>;

CodeRuntime* getCodeRuntime(_PyInterpreterFrame* frame) {
  BorrowedRef<PyFunctionObject> func = jitFrameGetFunction(frame);

  // Frame reification can look up runtime state without entering through a
  // guarded top-level JIT entrypoint.
  FreeThreadedJITEntrypointGuard guard;
  return cinderx::getModuleState()->jit_context->lookupCodeRuntime(func);
}

#if PY_VERSION_HEX >= 0x030E0000

void updatePrevInstr(_PyInterpreterFrame* frame);

int reifyRunningFrame(
    _PyInterpreterFrame* frame,
    [[maybe_unused]] PyObject* reifier) {
  jitFramePopulateFrame(frame);
  updatePrevInstr(frame);
  return 0;
}

#endif

bool isJitFrame(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES

#if PY_VERSION_HEX >= 0x030E0000
  PyObject* code = PyStackRef_AsPyObjectBorrow(frame->f_executable);
  return PyUnstable_JITExecutable_Check(code) &&
      ((PyUnstable_PyJitExecutable*)code)->je_reifier == &reifyRunningFrame;
#else
  return frameFunction(frame) == cinderx::getModuleState()->frame_reifier;
#endif

#else
  throw std::runtime_error{"isJitFrame: Lightweight frames are not supported"};
#endif
}

bool isGeneratorFrame(_PyInterpreterFrame* frame) {
  return frame->owner == FRAME_OWNED_BY_GENERATOR;
}

bool isInlined(_PyInterpreterFrame* frame) {
  if (isGeneratorFrame(frame)) {
    // Generator frames are never inlined
    return false;
  }

  return isInlinedFrame(frame);
}

#if defined(CINDER_X86_64)
// Return the base of the stack frame given its frame.
uintptr_t getFrameBaseFromOnStackFrame(_PyInterpreterFrame* frame) {
  // The frame is embedded in the frame header at the beginning of the
  // stack frame.
  return reinterpret_cast<uintptr_t>(frame) +
      sizeof(PyObject*) * _PyFrame_GetCode(frame)->co_framesize;
}

uintptr_t getIP(_PyInterpreterFrame* frame, int frame_size) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  JIT_CHECK(isJitFrame(frame), "frame not executed by the JIT");
  uintptr_t frame_base;
  if (isGeneratorFrame(frame)) {
    PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
    auto footer = jitGenDataFooter(gen);
    if (footer->yieldPoint == nullptr) {
      // The generator is running.
      // On x86, we read the return address from a fixed offset on the real
      // stack relative to the resume function's RBP.
      frame_base = footer->originalFramePointer;
    } else {
      // The generator is suspended.
      return footer->yieldPoint->resumeTarget();
    }
  } else {
    frame_base = getFrameBaseFromOnStackFrame(frame);
  }
  // Read the saved IP from the stack.
  // On x86, `call` pushes the return address on the stack at a fixed
  // location relative to the caller's frame pointer.
  uintptr_t ip;
  auto saved_ip =
      reinterpret_cast<uintptr_t*>(frame_base - frame_size - kPointerSize);
  memcpy(&ip, saved_ip, kPointerSize);
  return ip;
#else
  throw std::runtime_error{"getIP: Lightweight frames are not supported"};
#endif
}
#endif

void setIP(
    [[maybe_unused]] _PyInterpreterFrame* frame,
    [[maybe_unused]] int frame_size,
    [[maybe_unused]] uintptr_t new_ip) {
#if defined(__x86_64__) && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  JIT_CHECK(isJitFrame(frame), "frame not executed by the JIT");
  uintptr_t frame_base;
  if (isGeneratorFrame(frame)) {
    PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
    auto footer = jitGenDataFooter(gen);
    frame_base = footer->originalFramePointer;
  } else {
    frame_base = getFrameBaseFromOnStackFrame(frame);
  }
  auto saved_ip_addr =
      reinterpret_cast<uintptr_t*>(frame_base - frame_size - kPointerSize);
  *saved_ip_addr = new_ip;
#endif
}

// Collect all the frames in the unit, with the frame for the
// non-inlined function as the first element in the return vector.
std::vector<_PyInterpreterFrame*> getUnitFrames(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  std::vector<_PyInterpreterFrame*> frames;
  while (frame != nullptr) {
    if (!isJitFrame(frame)) {
      // We've reached an interpreter frame before finding the non-inlined
      // frame.
      JIT_ABORT("couldn't find non-inlined frame");
    }
    frames.emplace_back(frame);
    if (!isInlined(frame)) {
      std::reverse(frames.begin(), frames.end());
      return frames;
    }
    frame = frame->previous;
  }
  // We've walked entire stack without finding the non-inlined frame.
  JIT_ABORT("couldn't find non-inlined frame");
#else
  throw std::runtime_error{
      "getUnitFrames: Lightweight frames are not supported"};
#endif
}

UnitState getUnitState(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  std::vector<_PyInterpreterFrame*> unit_frames = getUnitFrames(frame);
  // Look up bytecode offsets for the frames in the unit.
  //
  // This is accomplished by combining a few different things:
  //
  // 1. For each unit, the JIT maintains a mapping of addresses in the
  //    generated code to code locations (code object, bytecode offset) for
  //    each active Python frame at that point, including frames for inlined
  //    functions.
  // 2. Every unit has a fixed-size native stack frame whose size is known at
  //    compile-time. This is recorded in the CodeRuntime for the unit.
  // 3. We can recover the CodeRuntime for a unit from its interpreter frame.
  // 4. We can recover the base of a unit's native stack frame from its
  //    frames. Frames for non-generator units are stored in the unit's
  //    native frame at a fixed offset from the base, while the frame base is
  //    stored directly in the JIT data for the generator.
  //
  UnitState unit_state;
  unit_state.reserve(unit_frames.size());
  _PyInterpreterFrame* non_inlined_sf = unit_frames[0];
  CodeRuntime* code_rt = getCodeRuntime(non_inlined_sf);
  JIT_CHECK(code_rt != nullptr, "failed to find code runtime");

  auto logUnitFrames = [&unit_frames] {
    JIT_LOG("Unit frames (increasing order of inline depth):");
    for (_PyInterpreterFrame* sf : unit_frames) {
      JIT_LOG("code={}", codeName(_PyFrame_GetCode(sf)));
    }
  };

#if defined(CINDER_AARCH64)
  // Look up bytecode offsets using the deopt index stored in the frame header.
  // The JIT updates this index before each instruction that can deopt, so it
  // always reflects the current position in the bytecode.
  std::size_t deopt_idx = jitFrameGetHeader(non_inlined_sf)->deopt_idx;
  std::optional<UnitCallStack> locs =
      code_rt->getUnitCallStackFromDeoptIdx(deopt_idx);
  if (locs.has_value()) {
    // We may have a different number of unit_frames than locs, this happens
    // when we're updating the outer frame while we're in an inlined function,
    // but our code objects should all line up.
    for (std::size_t i = 0; i < unit_frames.size(); i++) {
      JIT_DCHECK(
          _PyFrame_GetCode(unit_frames[i]) == locs->at(i).code,
          "code mismatch {} vs {}",
          codeName(_PyFrame_GetCode(unit_frames[i])),
          codeName(locs->at(i).code));
      unit_state.emplace_back(unit_frames[i], locs->at(i));
    }
  } else {
    // We might not have debug info for a number of reasons.
    // The consequences of getting this wrong (incorrect line numbers) don't
    // warrant aborting in production, but it is worth investigating.
    JIT_LOG(
        "No debug info for deopt_idx {} in {}",
        deopt_idx,
        PyUnicode_AsUTF8(code_rt->code()->co_qualname));
    logUnitFrames();
    JIT_DABORT("No debug info for deopt_idx {}", deopt_idx);
    for (_PyInterpreterFrame* unit_frame : unit_frames) {
      unit_state.emplace_back(
          unit_frame, CodeObjLoc{_PyFrame_GetCode(unit_frame), BCOffset{-1}});
    }
  }
#elif defined(CINDER_X86_64)
  // On x86-64, look up bytecode offsets using the IP-based symbolizer.
  uintptr_t ip = getIP(non_inlined_sf, code_rt->frameSize());
  std::optional<UnitCallStack> locs =
      code_rt->debugInfo()->getUnitCallStack(ip);
  if (locs.has_value()) {
    for (std::size_t i = 0; i < unit_frames.size(); i++) {
      JIT_DCHECK(
          _PyFrame_GetCode(unit_frames[i]) == locs->at(i).code,
          "code mismatch {} vs {}",
          codeName(_PyFrame_GetCode(unit_frames[i])),
          codeName(locs->at(i).code));
      unit_state.emplace_back(unit_frames[i], locs->at(i));
    }
  } else {
    JIT_LOG(
        "No debug info for addr {:x} {}",
        ip,
        symbolize(reinterpret_cast<void*>(ip)).value_or("no symbol"));
    logUnitFrames();
    JIT_DABORT("No debug info for addr {:x}", ip);
    for (_PyInterpreterFrame* unit_frame : unit_frames) {
      unit_state.emplace_back(
          unit_frame, CodeObjLoc{_PyFrame_GetCode(unit_frame), BCOffset{-1}});
    }
  }
#else
  CINDER_UNSUPPORTED
#endif

  return unit_state;
#else
  throw std::runtime_error{
      "updatePrevInstr: Lightweight frames are not supported"};
#endif
}

void updatePrevInstr(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  // Skip if IP was already patched to a deopt exit (no debug info there).
  if (jitFrameGetHeader(frame)->frame_status & JIT_FRAME_DEOPT_PATCHED) {
    return;
  }
  auto unit_state = getUnitState(frame);
  for (auto it = unit_state.rbegin(); it != unit_state.rend(); ++it) {
    _PyInterpreterFrame* cur_frame = it->frame;
    auto loc = it->loc.instr_offset;
    _Py_CODEUNIT* new_loc =
        _PyCode_CODE(_PyFrame_GetCode(cur_frame)) + loc.asIndex().value();
    JIT_DCHECK(
        new_loc >= (_Py_CODEUNIT*)(_PyCode_CODE(_PyFrame_GetCode(cur_frame))),
        "code prev instr underflow");
    JIT_DCHECK(
        new_loc < (_Py_CODEUNIT*)(_PyCode_CODE(_PyFrame_GetCode(cur_frame)) +
                                  Py_SIZE(_PyFrame_GetCode(cur_frame))),
        "code prev instr overflow");
    setFrameInstruction(cur_frame, new_loc);
  }
#else
  throw std::runtime_error{
      "updatePrevInstr: Lightweight frames are not supported"};
#endif
}

#if defined(META_PYTHON) && defined(Py_GIL_DISABLED)
CodeRuntime* lookupCodeRuntimeForOwningFrame(_PyInterpreterFrame* frame) {
  JIT_CHECK(
      !isInlinedFrame(frame),
      "owning frame unexpectedly refers to an inlined frame");
  return getCodeRuntime(frame);
}

struct ActiveDeoptMetadata {
  const DeoptMetadata* meta;
  uintptr_t frame_base;
};

std::optional<ActiveDeoptMetadata> getActiveDeoptMetadata(
    _PyInterpreterFrame* owning_frame,
    CodeRuntime* code_rt = nullptr) {
  if (code_rt == nullptr) {
    code_rt = lookupCodeRuntimeForOwningFrame(owning_frame);
  }
  if (code_rt == nullptr || code_rt->frameSize() < 0) {
    return std::nullopt;
  }

  std::size_t deopt_idx = jitFrameGetHeader(owning_frame)->deopt_idx;
  if (deopt_idx >= code_rt->deoptMetadatas().size()) {
    return std::nullopt;
  }
  uintptr_t frame_base;
  if (isGeneratorFrame(owning_frame)) {
    PyGenObject* gen = _PyGen_GetGeneratorFromFrame(owning_frame);
    auto footer = jitGenDataFooter(gen);
    frame_base = reinterpret_cast<uintptr_t>(footer);
  } else {
#if defined(CINDER_X86_64)
    frame_base = getFrameBaseFromOnStackFrame(owning_frame);
#else
    frame_base = reinterpret_cast<uintptr_t>(owning_frame) +
        sizeof(PyObject*) * _PyFrame_GetCode(owning_frame)->co_framesize;
#endif
  }
  return ActiveDeoptMetadata{
      .meta = &code_rt->getDeoptMetadata(deopt_idx),
      .frame_base = frame_base,
  };
}

int visitJitDeferredRefs(PyInterpreterState* interp, gcvisitobjects_t visit) {
  _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
    for (_PyInterpreterFrame* frame = p->current_frame; frame != nullptr;
         frame = frame->previous) {
      if (frame->owner >= FRAME_OWNED_BY_INTERPRETER) {
        continue;
      }

      if (!isJitFrame(frame) || isInlined(frame)) {
        continue;
      }

      std::optional<ActiveDeoptMetadata> active = getActiveDeoptMetadata(frame);
      if (!active.has_value()) {
        JIT_ABORT("no active deopt metadata");
      }

      visitLiveDeferredRefs(*active->meta, active->frame_base, visit);
    }
  }
  _Py_FOR_EACH_TSTATE_END(interp);
  return 0;
}
#endif

#if PY_VERSION_HEX < 0x030E0000

const PyMemberDef framereifier_members[] = {
    {"__vectorcalloffset__",
     T_PYSSIZET,
     offsetof(JitFrameReifier, vectorcall),
     READONLY,
     nullptr},
    {} /* Sentinel */
};

PyObject* framereifier_tpcall(PyObject*, PyObject* args, PyObject*) {
  if (PyTuple_GET_SIZE(args) != 1 ||
      !PyLong_CheckExact(PyTuple_GET_ITEM(args, 0))) {
    PyErr_SetString(PyExc_RuntimeError, "expected 1 arg of interpreter frame");
    return nullptr;
  }

  _PyInterpreterFrame* frame = reinterpret_cast<_PyInterpreterFrame*>(
      PyLong_AsVoidPtr(PyTuple_GET_ITEM(args, 0)));
  jitFramePopulateFrame(frame);
  updatePrevInstr(frame);

  return Py_NewRef(jitFrameGetFunction(frame));
}

PyType_Slot framereifier_type_slots[] = {
    {Py_tp_members, (void*)&framereifier_members},
    {Py_tp_call, (void*)&framereifier_tpcall},
    {0, 0}};

#endif

} // namespace

Ref<> makeFrameReifier([[maybe_unused]] BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  PyObject* reifier =
      PyUnstable_MakeJITExecutable(reifyRunningFrame, code, nullptr);
  if (reifier == nullptr) {
    PyErr_Print();
    throw std::runtime_error(
        fmt::format("failed to make reifier {}", codeQualname(code)));
  }
  return Ref<>::steal(reifier);
#endif
  return nullptr;
}

#if defined(META_PYTHON) && defined(Py_GIL_DISABLED)
void registerJitGCDeferredRefVisitor(PyInterpreterState* interp) {
  CiUnstable_GC_SetJITDeferredRefVisitor(interp, visitJitDeferredRefs);
}

void clearJitGCDeferredRefVisitor(PyInterpreterState* interp) {
  CiUnstable_GC_SetJITDeferredRefVisitor(interp, nullptr);
}
#endif

#if PY_VERSION_HEX < 0x030E0000

PyObject* jitFrameReifierVectorcall(
    JitFrameReifier*,
    PyObject* const* args,
    size_t nargsf) {
  if (PyVectorcall_NARGS(nargsf) != 1 || !PyLong_CheckExact(args[0])) {
    PyErr_SetString(PyExc_RuntimeError, "expected 1 arg of interpreter frame");
    return nullptr;
  }

  _PyInterpreterFrame* frame =
      reinterpret_cast<_PyInterpreterFrame*>(PyLong_AsVoidPtr(args[0]));
  jitFramePopulateFrame(frame);
  updatePrevInstr(frame);

  return Py_NewRef(jitFrameGetFunction(frame));
}

PyType_Spec JitFrameReifier_Spec = {
    .name = "cinderx.JitFrameReifier",
    // We store our pointer to JIT data in an additional variable slot at the
    // end of the object.
    .basicsize = sizeof(JitFrameReifier),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE |
        Py_TPFLAGS_HAVE_VECTORCALL,
    .slots = framereifier_type_slots,
};

#endif

void jitFramePopulateFrame([[maybe_unused]] _PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  if (jitFrameGetHeader(frame)->frame_status & JIT_FRAME_INITIALIZED) {
    // already fixed up
    return;
  }

#if Py_GIL_DISABLED
  // The TLBC index should be 0 i.e. the default code for the frame. This makes
  // sense because it's what the JIT is effectively running. Furthermore, if
  // something did set the TLBC index, we have no guarantee this ran on the
  // correct thread.
  JIT_CHECK(frame->tlbc_index == 0, "frame has non-zero tlbc_index");
#endif

  PyFunctionObject* func = jitFrameGetFunction(frame);

  BorrowedRef<PyCodeObject> code = frameCode(frame);
  frame->f_builtins = func->func_builtins;
  frame->f_globals = func->func_globals;
  frame->f_locals = nullptr;
#if PY_VERSION_HEX >= 0x030E0000
  frame->stackpointer = frame->localsplus + code->co_nlocalsplus;
#ifdef Py_DEBUG
  frame->lltrace = 0;
#endif
#else
  frame->stacktop = code->co_nlocalsplus;
#endif
  frame->frame_obj = nullptr;
  frame->return_offset = 0;
  int free_offset = code->co_nlocalsplus - numFreevars(code);
  Ci_STACK_TYPE* localsplus = &frame->localsplus[0];
  for (std::size_t i = 0; i < free_offset; i++) {
    *localsplus = Ci_STACK_NULL;
    localsplus++;
  }

  jitFrameGetHeader(frame)->frame_status |= JIT_FRAME_INITIALIZED;
#endif
}

// Uninstalls the frame reifier from the frame, replacing it with the
// original code object. This is used after the function is no longer
// executing but the frame is going to still survice - either because
// we've deopted or the PyFrameObject was materialized and the frame
// is going to be transferred there.
void jitFrameRemoveReifier(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* code = PyStackRef_AsPyObjectBorrow(frame->f_executable);
  if (PyUnstable_JITExecutable_Check(code)) {
    _PyStackRef existing = frame->f_executable;
    frame->f_executable = PyStackRef_FromPyObjectNew(
        ((PyUnstable_PyJitExecutable*)code)->je_code);
    PyStackRef_CLOSE(existing);
  }
#else
  // We no longer own the frame and need to provide a proper function for the
  // interpreter.
  if (!isInlinedFrame(frame)) {
    // ownership is transferred
    if (jitFrameGetFunction(frame) != nullptr) {
      setFrameFunction(frame, jitFrameGetFunction(frame));
      jitFrameGetHeader(frame)->frame_status = JIT_FRAME_INITIALIZED;
    }
  } else {
    // function isn't incref'd for inline frames, it's kept alive
    // by the code runtime.
    auto func = jitFrameGetFunction(frame);
    JIT_DCHECK(func != nullptr, "should have a func for inlined functions");
    setFrameFunction(frame, Py_NewRef(func));
  }
#endif
#endif
}

_PyInterpreterFrame* convertInterpreterFrameFromStackToSlab(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame) {
  PyCodeObject* code = _PyFrame_GetCode(frame);
  _PyInterpreterFrame* new_frame =
      _PyThreadState_PushFrame(tstate, code->co_framesize);
  if (new_frame == nullptr) {
    return nullptr;
  }

  jitFramePopulateFrame(frame);
  jitFrameRemoveReifier(frame);

  memcpy(new_frame, frame, code->co_framesize * sizeof(PyObject*));

  if (new_frame->frame_obj != nullptr) {
    new_frame->frame_obj->f_frame = new_frame;
  }

  // Non-generator frames don't incref f_funcobj during setup. Now that
  // we're handing the frame to the interpreter (which will decref
  // f_funcobj via _PyFrame_ClearExceptCode), incref to maintain the
  // balance. On 3.12+LW, jitFrameRemoveReifier already handles inlined
  // frames via Py_NewRef, so only non-inlined need the incref there.
#if PY_VERSION_HEX >= 0x030E0000
  Py_INCREF(PyStackRef_AsPyObjectBorrow(new_frame->f_funcobj));
#elif !defined(ENABLE_LIGHTWEIGHT_FRAMES)
  Py_INCREF(new_frame->f_funcobj);
#else
  if (!isInlinedFrame(frame)) {
    Py_INCREF(new_frame->f_funcobj);
  }
#endif

  return new_frame;
}

FrameHeader* jitFrameGetHeader(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  if (_PyFrame_GetCode(frame)->co_flags & kCoFlagsAnyGenerator) {
    PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
    auto footer = jitGenDataFooter(gen);
    return (FrameHeader*)&footer->frame_header;
  }
  return reinterpret_cast<FrameHeader*>(frame) - 1;
#else
  throw std::runtime_error{
      "jitFrameGetHeader: Lightweight frames are not supported"};
#endif
}

#if PY_VERSION_HEX < 0x030E0000
void jitFrameSetFunction(_PyInterpreterFrame* frame, PyFunctionObject* func) {
  jitFrameGetHeader(frame)->func = func;
}
#endif

BorrowedRef<PyFunctionObject> jitFrameGetFunction(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    return frameFunction(frame);
  }
  return reinterpret_cast<PyFunctionObject*>(
      jitFrameGetHeader(frame)->frame_status & ~JIT_FRAME_MASK);
#else
  return frameFunction(frame);
#endif
}

bool isInlinedFrame(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  return jitFrameGetHeader(frame)->frame_status & JIT_FRAME_INLINED;
#else
  return false;
#endif
}

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
void jitFrameInitLightweight(
    [[maybe_unused]] PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyFunctionObject* func,
    PyCodeObject* code,
    _frameowner owner,
    _PyInterpreterFrame* previous,
    [[maybe_unused]] PyObject* reifier) {
  // We must set `frame->owner` before calling `jitFrameSetFunction()`,
  // otherwise assertions in callees will fail if the code object has
  // generator-like flags but the frame's owner is not
  // `FRAME_OWNED_BY_GENERATOR`.
  frame->owner = owner;
  // Outside of tests this function is only used for generator frames. These
  // fields need to be set these for generators to help enable reuse of existing
  // CPython generator management. Particularly, having a more fully configured
  // frame allows us to use gen_traverse().
  frame->f_locals = nullptr;
  frame->frame_obj = nullptr;
#if PY_VERSION_HEX >= 0x030E0000
  JIT_DCHECK(reifier, "reifier needed for lightweight frames");
  frame->stackpointer = frame->localsplus;
  setFrameInstruction(frame, _PyCode_CODE(code));
#ifdef Py_GIL_DISABLED
  frame->tlbc_index = 0;
#endif
  setFrameCode(frame, reifier);
  setFrameFunction(frame, (PyObject*)Py_NewRef(func));
  jitFrameGetHeader(frame)->frame_status = 0;
#if defined(CINDER_AARCH64)
  jitFrameGetHeader(frame)->deopt_idx = 0;
#endif
#else
  frame->stacktop = 0;
  setFrameInstruction(frame, _PyCode_CODE(code) - 1);
  frame->prev_instr = _PyCode_CODE(code) - 1;
  setFrameCode(frame, (PyObject*)code);
  JIT_DCHECK(
      _Py_IsImmortal(cinderx::getModuleState()->frame_reifier),
      "frame helper must be immortal");
  setFrameFunction(frame, cinderx::getModuleState()->frame_reifier);
  jitFrameSetFunction(frame, (PyFunctionObject*)Py_NewRef(func));
#endif
  frame->previous = previous;
}
#endif

void jitFrameInitNormal(
    [[maybe_unused]] PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyFunctionObject* func,
    PyCodeObject* code,
    int null_locals_from,
    _frameowner owner,
    _PyInterpreterFrame* previous) {
#if PY_VERSION_HEX >= 0x030E0000
  _PyFrame_Initialize(
      tstate,
      frame,
      PyStackRef_FromPyObjectNew(func),
      NULL,
      code,
      null_locals_from,
      previous);
#else
  _PyFrame_Initialize(
      frame,
      (PyFunctionObject*)Py_NewRef(func),
      nullptr,
      code,
      null_locals_from);
  frame->previous = previous;
#endif
  // We must set `frame->owner` after calling `_PyFrame_Initialize`;
  // `PyFrame_Initialize` sets `frame->owner` to `FRAME_OWNED_BY_THREAD`,
  // potentially overriding any value we set earlier.
  frame->owner = owner;
}

void jitFrameInit(
    [[maybe_unused]] PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyFunctionObject* func,
    PyCodeObject* code,
    int null_locals_from,
    _frameowner owner,
    _PyInterpreterFrame* previous,
    PyObject* reifier) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  jitFrameInitLightweight(tstate, frame, func, code, owner, previous, reifier);
#else
  jitFrameInitNormal(
      tstate, frame, func, code, null_locals_from, owner, previous);
#endif
}

void jitFrameClearExceptCode(_PyInterpreterFrame* frame) {
  /* It is the responsibility of the owning generator/coroutine
   * to have cleared the enclosing generator, if any. */
  JIT_DCHECK(
      frame->owner != FRAME_OWNED_BY_GENERATOR ||
          _PyGen_GetGeneratorFromFrame(frame)->gi_frame_state == FRAME_CLEARED,
      "bad frame state");
  // GH-99729: Clearing this frame can expose the stack (via finalizers). It's
  // crucial that this frame has been unlinked, and is no longer visible:
  JIT_DCHECK(currentFrame(PyThreadState_Get()) != frame, "wrong current frame");

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  // If we've already been requested by the runtime to initialize this
  // _PyInterpreterFrame then we just fall back to its implementation to
  // handle the clearing.
  if (jitFrameGetHeader(frame)->frame_status & JIT_FRAME_INITIALIZED) {
    jitFrameRemoveReifier(frame);
    _PyFrame_ClearExceptCode(frame);
    return;
  }

  // Otherwise only clear things that we've initialized.
  BorrowedRef<PyCodeObject> code = frameCode(frame);
  int free_offset = code->co_nlocalsplus - numFreevars(code);
  for (int i = free_offset; i < code->co_nlocalsplus; i++) {
    Ci_STACK_CLEAR(frame->localsplus[i]);
  }
  Ci_STACK_CLOSE(frame->f_funcobj);
#if PY_VERSION_HEX < 0x030E0000
  // We can't leave our reifier dangling here otherwise we may
  // continue to get callbacks, instead leave the function dangling.
  frame->f_funcobj = jitFrameGetFunction(frame);
  // function isn't incref'd for inline frames, it's kept alive
  // by the code runtime.
  if (!isInlinedFrame(frame)) {
    Py_XDECREF(jitFrameGetFunction(frame));
    jitFrameGetHeader(frame)->frame_status = JIT_FRAME_INITIALIZED;
  }
#endif
#else
  _PyFrame_ClearExceptCode(frame);
#endif
}

// If the frame is JITed returns the function object. Returns null otherwise.
// On the OSS build we can't distinguish between JITed and non-JITed frames
// because we don't have the reifier so always returns the function object.
BorrowedRef<PyFunctionObject> getFrameFunctionIfJitted(
    _PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  if (isJitFrame(frame)) {
    return jitFrameGetFunction(frame);
  }
  return nullptr;
#else
  // In the OSS build we can't distinguish between JITed and non-JITed frames
  return frameFunction(frame);
#endif
}

void retainActiveDeferredData(
    std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>>& pending,
    std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>>&
        still_active) {
  PyInterpreterState* interp = PyInterpreterState_Get();

#ifdef Py_GIL_DISABLED
  _PyEval_StopTheWorld(interp);
#endif

  PyThreadState* ts = PyInterpreterState_ThreadHead(interp);
  while (ts != nullptr) {
    _PyInterpreterFrame* frame = currentFrame(ts);
    while (frame != nullptr) {
      // Skip incomplete frames (interpreter/C-stack entry frames). Their
      // f_executable can spuriously satisfy isJitFrame() on 3.14+ while
      // f_funcobj still holds sentinel garbage, so treating them as JIT frames
      // would dereference an invalid function. Matches visitJitDeferredRefs().
#if PY_VERSION_HEX >= 0x030E0000
      if (frame->owner >= FRAME_OWNED_BY_INTERPRETER) {
#else
      if (frame->owner >= FRAME_OWNED_BY_CSTACK) {
#endif
        frame = frame->previous;
        continue;
      }
      BorrowedRef<PyFunctionObject> func = getFrameFunctionIfJitted(frame);
      // We can have trampoline frame pushed by _PyFrame_PushTrampolineUnchecked
      // whose funcobj is Py_None. Ignore these as they have no code to keep
      // alive.
      if (func != nullptr && func != Py_None) {
        OwnedCompilationKey key{func};
        auto it = pending.find(key);
        if (it != pending.end()) {
          still_active.emplace(std::move(key), std::move(it->second));
          pending.erase(it);
        }
      }
      frame = frame->previous;
    }
    ts = PyThreadState_Next(ts);
  }

#ifdef Py_GIL_DISABLED
  _PyEval_StartTheWorld(interp);
#endif
}

void deoptAllJitFramesOnStack() {
#if defined(__x86_64__) && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  PyInterpreterState* interp = PyInterpreterState_Get();

  // In free-threaded builds, other threads are actively running and may be
  // reading/writing the same frame data we're about to patch. Stop all
  // threads before modifying their stack frames.
#ifdef Py_GIL_DISABLED
  _PyEval_StopTheWorld(interp);
#endif

  PyThreadState* ts = PyInterpreterState_ThreadHead(interp);

  while (ts != nullptr) {
    _PyInterpreterFrame* frame = currentFrame(ts);

    // Only skip the topmost frame for the current thread — it's the
    // instrumentation-activation call (e.g. register_callback), not a
    // frame we need to deopt. Other threads' topmost frames may be
    // looping JIT code stopped at the eval breaker.
    if (ts == PyThreadState_Get() && frame != nullptr) {
      frame = frame->previous;
    }

    // Track innermost inlined frame. updatePrevInstr must be called on
    // the innermost frame so it walks back and updates all inlined frames.
    _PyInterpreterFrame* innermost_inlined = nullptr;

    while (frame != nullptr) {
      if (isJitFrame(frame)) {
        // Only patch non-inlined frames — inlined frames share the outer
        // frame's stack and saved return address.
        if (!isInlined(frame)) {
          CodeRuntime* code_rt = getCodeRuntime(frame);
          if (code_rt != nullptr) {
            uintptr_t current_ip = getIP(frame, code_rt->frameSize());
            auto deopt_exit = code_rt->getCallsiteDeoptExit(current_ip);
            if (deopt_exit.has_value()) {
              // Set prev_instr/instr_ptr BEFORE patching the IP — after
              // patching, the IP points to a deopt exit with no debug info.
              // Use innermost_inlined if present so updatePrevInstr walks
              // back and updates all inlined frames in this unit.
              if (innermost_inlined != nullptr) {
                updatePrevInstr(innermost_inlined);
                innermost_inlined = nullptr;
              } else {
                updatePrevInstr(frame);
              }

              jitFramePopulateFrame(frame);
              setIP(frame, code_rt->frameSize(), deopt_exit.value());

              // Mark frame so updatePrevInstr skips it (deopt exit IP
              // has no debug info entry).
              jitFrameGetHeader(frame)->frame_status |= JIT_FRAME_DEOPT_PATCHED;
            } else {
              JIT_DLOG(
                  "No callsite deopt exit for JIT frame at IP {:#x}",
                  current_ip);
              innermost_inlined = nullptr;
            }
          } else {
            innermost_inlined = nullptr;
          }
        } else {
          // Inlined JIT frame — remember the innermost one for
          // updatePrevInstr when we reach the outer frame.
          if (innermost_inlined == nullptr) {
            innermost_inlined = frame;
          }
        }
      } else {
        // Non-JIT frame — reset inlined tracking since the next
        // non-inlined JIT frame belongs to a different unit.
        innermost_inlined = nullptr;
      }
      frame = frame->previous;
    }

    ts = PyThreadState_Next(ts);
  }

#ifdef Py_GIL_DISABLED
  _PyEval_StartTheWorld(interp);
#endif

#endif
}

} // namespace cinderx::jit
