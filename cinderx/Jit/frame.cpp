// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "internal/pycore_frame.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

namespace jit {

namespace {

struct FrameAndLoc {
  FrameAndLoc(_PyInterpreterFrame* sf, const CodeObjLoc& l)
      : frame(sf), loc(l) {}
  _PyInterpreterFrame* frame;
  CodeObjLoc loc;
};

using UnitState = std::vector<FrameAndLoc>;

CodeRuntime* getCodeRuntime(_PyInterpreterFrame* frame) {
  BorrowedRef<PyFunctionObject> func;
  if (hasRtfsFunction(frame)) {
    auto rtfs = jitFrameGetRtfs(frame);
    JIT_DCHECK(rtfs != nullptr, "RuntimeFrameState should have a function");
    func = rtfs->func();
  } else {
    func = jitFrameGetFunction(frame);
  }

  return cinderx::getModuleState()->jitContext()->lookupCodeRuntime(func);
}

#if PY_VERSION_HEX >= 0x030E0000

void updatePrevInstr(_PyInterpreterFrame* frame);

int reifyRunningFrame(_PyInterpreterFrame* frame, PyObject* reifier) {
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
  return frameFunction(frame) == cinderx::getModuleState()->frameReifier();
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

  // Inlined functions have a RTFS, non-inlined frames have a function
  return hasRtfsFunction(frame);
}

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
      frame_base = footer->originalFramePointer;
    } else {
      // The generator is suspended.
      return footer->yieldPoint->resumeTarget();
    }
  } else {
    frame_base = getFrameBaseFromOnStackFrame(frame);
  }
  // Read the saved IP from the stack
  uintptr_t ip;
  auto saved_ip =
      reinterpret_cast<uintptr_t*>(frame_base - frame_size - kPointerSize);
  memcpy(&ip, saved_ip, kPointerSize);
  return ip;
#else
  throw std::runtime_error{"getIP: Lightweight frames are not supported"};
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
  std::vector<_PyInterpreterFrame*> unit_frames = getUnitFrames(frame);
  auto logUnitFrames = [&unit_frames] {
    JIT_LOG("Unit frames (increasing order of inline depth):");
    for (_PyInterpreterFrame* sf : unit_frames) {
      JIT_LOG("code={}", codeName(_PyFrame_GetCode(sf)));
    }
  };
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
  uintptr_t ip = getIP(non_inlined_sf, code_rt->frameSize());
  std::optional<UnitCallStack> locs =
      code_rt->debugInfo()->getUnitCallStack(ip);
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
    // We might not have debug info for a number of reasons (e.g. we've read
    // the return address incorrectly or there's a bug with how we're
    // generating the information). The consequences of getting this wrong
    // (incorrect line numbers) don't warrant aborting in production, but it is
    // worth investigating. Leave some breadcrumbs to help with debugging.
    JIT_LOG("No debug info for addr {:x}", ip);
    logUnitFrames();
    JIT_DABORT("No debug info for addr {:x}", ip);
    for (_PyInterpreterFrame* unit_frame : unit_frames) {
      unit_state.emplace_back(
          unit_frame, CodeObjLoc{_PyFrame_GetCode(unit_frame), BCOffset{-1}});
    }
  }

  return unit_state;
}

void updatePrevInstr(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
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
      PyLong_FromVoidPtr(PyTuple_GET_ITEM(args, 0)));
  jitFramePopulateFrame(frame);
  updatePrevInstr(frame);

  BorrowedRef<PyFunctionObject> func = hasRtfsFunction(frame)
      ? jitFrameGetRtfs(frame)->func()
      : jitFrameGetFunction(frame);
  return Py_NewRef(func);
}

PyType_Slot framereifier_type_slots[] = {
    {Py_tp_members, (void*)&framereifier_members},
    {Py_tp_call, (void*)&framereifier_tpcall},
    {0, 0}};

#endif

} // namespace

Ref<> makeFrameReifier([[maybe_unused]] BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  if (getConfig().frame_mode == FrameMode::kLightweight) {
    PyObject* reifier =
        PyUnstable_MakeJITExecutable(reifyRunningFrame, code, nullptr);
    if (reifier == nullptr) {
      PyErr_Print();
      throw std::runtime_error(
          fmt::format("failed to make reifier {}", codeQualname(code)));
    }
    return Ref<>::steal(reifier);
  }
#endif
  return nullptr;
}

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

  BorrowedRef<PyFunctionObject> func = hasRtfsFunction(frame)
      ? jitFrameGetRtfs(frame)->func()
      : jitFrameGetFunction(frame);
  return Py_NewRef(func);
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
  if (jitFrameGetHeader(frame)->rtfs & JIT_FRAME_INITIALIZED) {
    // already fixed up
    return;
  }

  PyFunctionObject* func;
  if (!hasRtfsFunction(frame)) {
    func = jitFrameGetFunction(frame);
  } else {
    RuntimeFrameState* rtfs = jitFrameGetRtfs(frame);
    func = rtfs->func();
    JIT_DCHECK(func != nullptr, "should have a func for inlined functions");
  }

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
  if (!(code->co_flags & kCoFlagsAnyGenerator)) {
    frame->owner = FRAME_OWNED_BY_THREAD;
  }
  int free_offset = code->co_nlocalsplus - numFreevars(code);
  Ci_STACK_TYPE* localsplus = &frame->localsplus[0];
  for (std::size_t i = 0; i < free_offset; i++) {
    *localsplus = Ci_STACK_NULL;
    localsplus++;
  }

  jitFrameGetHeader(frame)->rtfs |= JIT_FRAME_INITIALIZED;
#else
  throw std::runtime_error{
      "jitFramePopulateFrame: Lightweight frames are not supported"};
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
  if (!hasRtfsFunction(frame)) {
    // ownership is transferred
    setFrameFunction(frame, jitFrameGetFunction(frame));
  } else {
    RuntimeFrameState* rtfs = jitFrameGetRtfs(frame);
    auto func = rtfs->func();
    JIT_DCHECK(func != nullptr, "should have a func for inlined functions");
    setFrameFunction(frame, Py_NewRef(func));
  }
#endif
#else
  throw std::runtime_error{
      "jitFrameRemoveReifier: Lightweight frames are not supported"};
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

void jitFrameSetFunction(_PyInterpreterFrame* frame, PyFunctionObject* func) {
  jitFrameGetHeader(frame)->func = func;
}

BorrowedRef<PyFunctionObject> jitFrameGetFunction(_PyInterpreterFrame* frame) {
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    return frameFunction(frame);
  }
  return reinterpret_cast<PyFunctionObject*>(
      jitFrameGetHeader(frame)->rtfs & ~JIT_FRAME_MASK);
}

RuntimeFrameState* jitFrameGetRtfs(_PyInterpreterFrame* frame) {
  assert(hasRtfsFunction(frame));
  assert(!(frameCode(frame)->co_flags & kCoFlagsAnyGenerator));
  return reinterpret_cast<RuntimeFrameState*>(
      jitFrameGetHeader(frame)->rtfs & ~JIT_FRAME_MASK);
}

bool hasRtfsFunction(_PyInterpreterFrame* frame) {
  return jitFrameGetHeader(frame)->rtfs & JIT_FRAME_RTFS;
}

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
  setFrameCode(frame, reifier);
  setFrameFunction(frame, (PyObject*)Py_NewRef(func));
  jitFrameGetHeader(frame)->rtfs = 0;
#else
  frame->stacktop = 0;
  setFrameInstruction(frame, _PyCode_CODE(code) - 1);
  frame->prev_instr = _PyCode_CODE(code) - 1;
  setFrameCode(frame, (PyObject*)code);
  JIT_DCHECK(
      _Py_IsImmortal(cinderx::getModuleState()->frameReifier()),
      "frame helper must be immortal");
  setFrameFunction(frame, cinderx::getModuleState()->frameReifier());
  jitFrameSetFunction(frame, (PyFunctionObject*)Py_NewRef(func));
#endif
  frame->previous = previous;
}

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
  if (getConfig().frame_mode == FrameMode::kLightweight) {
    jitFrameInitLightweight(
        tstate, frame, func, code, owner, previous, reifier);
    return;
  }

  jitFrameInitNormal(
      tstate, frame, func, code, null_locals_from, owner, previous);
}

size_t jitFrameGetSize(PyCodeObject* code) {
  return code->co_framesize;
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

  if (getConfig().frame_mode != FrameMode::kLightweight) {
    _PyFrame_ClearExceptCode(frame);
    return;
  }

  // If we've already been requested by the runtime to initialize this
  // _PyInterpreterFrame then we just fall back to its implementation to
  // handle the clearing.
  if (jitFrameGetHeader(frame)->rtfs & JIT_FRAME_INITIALIZED) {
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
  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    if (!hasRtfsFunction(frame)) {
      Py_DECREF(jitFrameGetFunction(frame));
    }
  }
}

RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  return RuntimeFrameState{
      frameCode(frame), frame->f_builtins, frame->f_globals};
}

} // namespace jit

#endif
