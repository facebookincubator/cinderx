// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame.h"

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000

#include "cinder/exports.h"
#include "cinder/genobject_jit.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_shadow_frame.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_set>

static bool is_shadow_frame_for_gen(_PyShadowFrame* shadow_frame) {
  // TODO(bsimmers): This condition will need to change when we support eager
  // coroutine execution in the JIT, since there is no PyGenObject* for the
  // frame while executing eagerly (but isGen() will still return true).
  // TODO(T110700318): Collapse into RTFS case
  bool is_jit_gen = _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_CODE_RT &&
      static_cast<jit::CodeRuntime*>(_PyShadowFrame_GetPtr(shadow_frame))
          ->frameState()
          ->isGen();

  // Note this may be JIT or interpreted.
  bool is_gen_with_frame =
      _PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME &&
      _PyShadowFrame_GetPyFrame(shadow_frame)->f_gen != nullptr;
  return is_jit_gen || is_gen_with_frame;
}

namespace jit {

namespace {

const char* codeName(PyCodeObject* code) {
  if (code->co_qualname == nullptr) {
    return "<null>";
  }
  return PyUnicode_AsUTF8(code->co_qualname);
}

Ref<> getModuleName(_PyShadowFrame* shadow_frame) {
  RuntimeFrameState rtfs = runtimeFrameStateFromShadowFrame(shadow_frame);
  BorrowedRef<> globals = rtfs.globals();
  JIT_CHECK(
      globals != nullptr,
      "Shadow frame {} with kind {} has null globals",
      static_cast<void*>(shadow_frame),
      _PyShadowFrame_GetPtrKind(shadow_frame));
  auto result = Ref<>::create(PyDict_GetItemString(globals, "__name__"));
  if (result) {
    return result;
  }

  result = Ref<>::steal(PyUnicode_FromString("<unknown>"));
  JIT_DCHECK(
      result || PyErr_Occurred(),
      "Null result returned without a Python exception set");
  return result;
}

// Return the base of the stack frame given its shadow frame.
uintptr_t getFrameBaseFromOnStackShadowFrame(_PyShadowFrame* shadow_frame) {
  // The shadow frame is embedded in the frame header at the beginning of the
  // stack frame.
  return reinterpret_cast<uintptr_t>(shadow_frame) +
      offsetof(FrameHeader, shadow_frame) + sizeof(JITShadowFrame);
}

CodeRuntime* getCodeRuntime(_PyShadowFrame* shadow_frame) {
  JIT_CHECK(
      _PyShadowFrame_GetOwner(shadow_frame) == PYSF_JIT,
      "shadow frame not owned by the JIT");
  if (is_shadow_frame_for_gen(shadow_frame)) {
    // The shadow frame belongs to a generator; retrieve the CodeRuntime
    // directly from the generator.
    PyGenObject* gen = _PyShadowFrame_GetGen(shadow_frame);
    return reinterpret_cast<GenDataFooter*>(gen->gi_jit_data)->code_rt;
  }
  auto jit_sf = reinterpret_cast<JITShadowFrame*>(shadow_frame);
  _PyShadowFrame_PtrKind rt_ptr_kind = JITShadowFrame_GetRTPtrKind(jit_sf);
  JIT_CHECK(
      rt_ptr_kind == PYSF_CODE_RT, "unexpected ptr kind: {}", rt_ptr_kind);
  return reinterpret_cast<jit::CodeRuntime*>(JITShadowFrame_GetRTPtr(jit_sf));
}

// Find a shadow frame in the call stack. If the frame was found, returns the
// last Python frame seen during the search, or nullptr if there was none.
std::optional<PyFrameObject*> findInnermostPyFrameForShadowFrame(
    PyThreadState* tstate,
    _PyShadowFrame* needle) {
  PyFrameObject* prev_py_frame = nullptr;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;
  while (shadow_frame) {
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      prev_py_frame = _PyShadowFrame_GetPyFrame(shadow_frame);
    } else if (shadow_frame == needle) {
      return prev_py_frame;
    }
    shadow_frame = shadow_frame->prev;
  }
  return {};
}

// Return the instruction pointer for the JIT-compiled function that is
// executing shadow_frame.
uintptr_t getIP(_PyShadowFrame* shadow_frame, int frame_size) {
  JIT_CHECK(
      _PyShadowFrame_GetOwner(shadow_frame) == PYSF_JIT,
      "shadow frame not executed by the JIT");
  uintptr_t frame_base;
  if (is_shadow_frame_for_gen(shadow_frame)) {
    PyGenObject* gen = _PyShadowFrame_GetGen(shadow_frame);
    auto footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
    if (footer->yieldPoint == nullptr) {
      // The generator is running.
      frame_base = footer->originalRbp;
    } else {
      // The generator is suspended.
      return footer->yieldPoint->resumeTarget();
    }
  } else {
    frame_base = getFrameBaseFromOnStackShadowFrame(shadow_frame);
  }
  // Read the saved IP from the stack
  uintptr_t ip;
  auto saved_ip =
      reinterpret_cast<uintptr_t*>(frame_base - frame_size - kPointerSize);
  memcpy(&ip, saved_ip, kPointerSize);
  return ip;
}

// Create an unlinked PyFrameObject for the given shadow frame.
Ref<PyFrameObject> createPyFrame(
    PyThreadState* tstate,
    _PyShadowFrame* shadow_frame) {
  _PyShadowFrame_PtrKind kind = _PyShadowFrame_GetPtrKind(shadow_frame);

  JIT_CHECK(
      kind != PYSF_PYFRAME,
      "Shadow frame {} already has a Python frame",
      static_cast<void*>(shadow_frame));

  RuntimeFrameState rtfs = runtimeFrameStateFromShadowFrame(shadow_frame);
  JIT_CHECK(
      kind != PYSF_RTFS || !rtfs.isGen(),
      "Unexpected generator in inline shadow frame");

  PyFrameConstructor py_frame_ctor = {};
  py_frame_ctor.fc_globals = rtfs.globals();
  py_frame_ctor.fc_builtins = rtfs.builtins();
  py_frame_ctor.fc_code = rtfs.code();
  Ref<PyFrameObject> py_frame = Ref<PyFrameObject>::steal(
      _PyFrame_New_NoTrack(tstate, &py_frame_ctor, nullptr));
  _PyObject_GC_TRACK(py_frame);
  // _PyFrame_New_NoTrack links the frame into the thread stack.
  Py_CLEAR(py_frame->f_back);
  return py_frame;
}

void insertPyFrameBefore(
    PyThreadState* tstate,
    BorrowedRef<PyFrameObject> frame,
    BorrowedRef<PyFrameObject> cursor) {
  if (cursor == nullptr) {
    // Insert frame at the top of the call stack
    Py_XINCREF(tstate->frame);
    frame->f_back = tstate->frame;
    // ThreadState holds a borrowed reference
    tstate->frame = frame;
    return;
  }
  // Insert frame immediately before cursor in the call stack
  // New frame steals reference for cursor->f_back
  frame->f_back = cursor->f_back;
  // Need to create a new reference for cursor to the newly created frame.
  Py_INCREF(frame);
  cursor->f_back = frame;
}

void attachPyFrame(
    BorrowedRef<PyFrameObject> py_frame,
    _PyShadowFrame* shadow_frame) {
  if (is_shadow_frame_for_gen(shadow_frame)) {
    // Transfer ownership of the new reference to frame to the generator
    // epilogue.  It handles detecting and unlinking the frame if the generator
    // is present in the `data` field of the shadow frame.
    //
    // A generator may be resumed multiple times. If a frame is materialized in
    // one activation, all subsequent activations must link/unlink the
    // materialized frame on function entry/exit. There's no active signal in
    // these cases, so we're forced to check for the presence of the
    // frame. Linking is handled by `_PyJIT_GenSend`, while unlinking is
    // handled by either the epilogue or, in the event that the generator
    // deopts, the interpreter loop. In the future we may refactor things so
    // that `_PyJIT_GenSend` handles both linking and unlinking.
    PyGenObject* gen = _PyShadowFrame_GetGen(shadow_frame);
    // f_gen is borrowed
    py_frame->f_gen = reinterpret_cast<PyObject*>(gen);
    // gi_frame is owned
    gen->gi_frame = py_frame.get();
    Py_INCREF(py_frame);
  } else {
    // Save the original data field so that we can recover the the
    // CodeRuntime/RuntimeFrameState pointer if we need to later on.
    reinterpret_cast<JITShadowFrame*>(shadow_frame)->orig_data =
        shadow_frame->data;
  }
  shadow_frame->data =
      _PyShadowFrame_MakeData(py_frame, PYSF_PYFRAME, PYSF_JIT);
}

PyFrameState getPyFrameStateForJITGen(PyGenObject* gen) {
  JIT_DCHECK(gen->gi_jit_data != nullptr, "not a JIT generator");
  switch (Ci_GetJITGenState(gen)) {
    case Ci_JITGenState_JustStarted: {
      return FRAME_CREATED;
    }
    case Ci_JITGenState_Running:
    case Ci_JITGenState_Throwing: {
      return Ci_JITGenIsExecuting(gen) ? FRAME_EXECUTING : FRAME_SUSPENDED;
    }
    case Ci_JITGenState_Completed: {
      JIT_ABORT("completed generators don't have frames");
    }
  }
  JIT_ABORT("Invalid generator state");
}

// Ensure that a PyFrameObject with f_lasti equal to last_instr_offset exists
// for shadow_frame. If a new PyFrameObject is created it will be inserted
// at the position specified by cursor:
//
//   - nullptr      - Top of stack
//   - not-nullptr  - Immediately before cursor
//   - std::nullopt - Not inserted
//
// TODO(mpage): Use std::variant to represent the insertion position.
BorrowedRef<PyFrameObject> materializePyFrame(
    PyThreadState* tstate,
    _PyShadowFrame* shadow_frame,
    BCOffset last_instr_offset,
    std::optional<BorrowedRef<PyFrameObject>> cursor) {
  // Make sure a PyFrameObject exists at the correct location in the call
  // stack.
  BorrowedRef<PyFrameObject> py_frame;
  if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
    py_frame.reset(_PyShadowFrame_GetPyFrame(shadow_frame));
  } else {
    // Python frame doesn't exist yet, create it and insert it into the
    // call stack.
    Ref<PyFrameObject> new_frame = createPyFrame(tstate, shadow_frame);
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      // The frame was materialized between our initial check and here. This
      // can happen if the allocation in createPyFrame triggers GC and GC
      // invokes a finalizer that materializes the stack.
      py_frame.reset(_PyShadowFrame_GetPyFrame(shadow_frame));
    } else {
      // Ownership of the new reference is transferred to whomever unlinks the
      // frame (either the JIT epilogue, the interpreter loop, or the generator
      // send implementation).
      py_frame = new_frame.release();
      attachPyFrame(py_frame, shadow_frame);
      if (cursor.has_value()) {
        insertPyFrameBefore(tstate, py_frame, cursor.value());
      }
    }
  }
  // Update the PyFrameObject to refect the state of the JIT function
  py_frame->f_lasti = last_instr_offset.asIndex().value();
  if (is_shadow_frame_for_gen(shadow_frame)) {
    PyGenObject* gen = _PyShadowFrame_GetGen(shadow_frame);
    py_frame->f_state = getPyFrameStateForJITGen(gen);
  } else {
    py_frame->f_state = FRAME_EXECUTING;
  }
  return py_frame;
}

bool isInlined(_PyShadowFrame* shadow_frame) {
  if (_PyShadowFrame_GetOwner(shadow_frame) == PYSF_INTERP) {
    return false;
  }
  if (is_shadow_frame_for_gen(shadow_frame)) {
    return false;
  }
  auto jit_sf = reinterpret_cast<JITShadowFrame*>(shadow_frame);
  _PyShadowFrame_PtrKind rt_kind = JITShadowFrame_GetRTPtrKind(jit_sf);
  switch (rt_kind) {
    case PYSF_RTFS: {
      return true;
    }
    case PYSF_CODE_RT: {
      return false;
    }
    default: {
      JIT_ABORT("invalid ptr kind {} for rt", rt_kind);
    }
  }
}

struct ShadowFrameAndLoc {
  ShadowFrameAndLoc(_PyShadowFrame* sf, const CodeObjLoc& l)
      : shadow_frame(sf), loc(l) {}
  _PyShadowFrame* shadow_frame;
  CodeObjLoc loc;
};

// Collect all the shadow frames in the unit, with the shadow frame for the
// non-inlined function as the first element in the return vector.
std::vector<_PyShadowFrame*> getUnitFrames(_PyShadowFrame* shadow_frame) {
  JIT_CHECK(
      _PyShadowFrame_GetOwner(shadow_frame) == PYSF_JIT,
      "must pass jit-owned shadow frame");
  std::vector<_PyShadowFrame*> frames;
  while (shadow_frame != nullptr) {
    _PyShadowFrame_Owner owner = _PyShadowFrame_GetOwner(shadow_frame);
    switch (owner) {
      case PYSF_INTERP: {
        // We've reached an interpreter frame before finding the non-inlined
        // frame.
        JIT_ABORT("couldn't find non-inlined frame");
      }
      case PYSF_JIT: {
        frames.emplace_back(shadow_frame);
        if (!isInlined(shadow_frame)) {
          std::reverse(frames.begin(), frames.end());
          return frames;
        }
        break;
      }
    }
    shadow_frame = shadow_frame->prev;
  }
  // We've walked entire stack without finding the non-inlined frame.
  JIT_ABORT("couldn't find non-inlined frame");
}

// The shadow frames (non-inlined + inlined) and their respective code
// locations for a JIT unit. The non-inlined frame is the first element in
// the vector.
using UnitState = std::vector<ShadowFrameAndLoc>;

// Get the unit state for the JIT unit beginning at shadow_frame.
UnitState getUnitState(_PyShadowFrame* shadow_frame) {
  JIT_CHECK(
      _PyShadowFrame_GetOwner(shadow_frame) == PYSF_JIT,
      "must pass jit-owned shadow frame");
  std::vector<_PyShadowFrame*> unit_frames = getUnitFrames(shadow_frame);
  auto logUnitFrames = [&unit_frames] {
    JIT_LOG("Unit shadow frames (increasing order of inline depth):");
    for (_PyShadowFrame* sf : unit_frames) {
      JIT_LOG("code={}", codeName(_PyShadowFrame_GetCode(sf)));
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
  // 3. We can recover the CodeRuntime for a unit from its shadow frames.
  // 4. We can recover the base of a unit's native stack frame from its shadow
  //    frames. Shadow frames for non-generator units are stored in the unit's
  //    native frame at a fixed offset from the base, while the frame base is
  //    stored directly in the JIT data for the generator.
  //
  UnitState unit_state;
  unit_state.reserve(unit_frames.size());
  _PyShadowFrame* non_inlined_sf = unit_frames[0];
  CodeRuntime* code_rt = getCodeRuntime(non_inlined_sf);
  uintptr_t ip = getIP(non_inlined_sf, code_rt->frame_size());
  std::optional<UnitCallStack> locs =
      code_rt->debug_info()->getUnitCallStack(ip);
  if (locs.has_value()) {
    if (locs->size() != unit_frames.size()) {
      JIT_LOG("DebugInfo frames:");
      for (const CodeObjLoc& col : locs.value()) {
        JIT_LOG("code={} bc_off={}", codeName(col.code), col.instr_offset);
      }
      logUnitFrames();
      JIT_ABORT(
          "Size mismatch: expected {} frames but got {}",
          locs->size(),
          unit_frames.size());
    }
    for (std::size_t i = 0; i < unit_frames.size(); i++) {
      unit_state.emplace_back(unit_frames[i], locs->at(i));
    }
  } else {
    // We might not have debug info for a number of reasons (e.g. we've read
    // the return address incorrectly or there's a bug with how we're
    // generating the information). The consequences of getting this wrong
    // (incorrect line numbers) don't warrant aborting in production, but it is
    // worth investigating. Leave some breadcrumbs to help with debugging.
    JIT_LOG("No debug info for addr {}", ip);
    logUnitFrames();
    JIT_DABORT("No debug info for addr {:x}", ip);
    for (std::size_t i = 0; i < unit_frames.size(); i++) {
      _PyShadowFrame* sf = unit_frames[i];
      unit_state.emplace_back(
          sf, CodeObjLoc{_PyShadowFrame_GetCode(sf), BCOffset{-1}});
    }
  }

  return unit_state;
}

// Ensure that PyFrameObjects exist for each shadow frame in the unit, and that
// each PyFrameObject's f_lasti is updated to the offset for the corresponding
// shadow frame.
//
// If created, the PyFrameObjects are linked together, and the
// PyFrameObject for the innermost shadow frame is linked to cursor, if one is
// provided.
//
// Returns the PyFrameObject for the non-inlined shadow frame.
BorrowedRef<PyFrameObject> materializePyFrames(
    PyThreadState* tstate,
    const UnitState& unit_state,
    std::optional<BorrowedRef<PyFrameObject>> cursor) {
  for (auto it = unit_state.rbegin(); it != unit_state.rend(); ++it) {
    cursor = materializePyFrame(
        tstate, it->shadow_frame, it->loc.instr_offset, cursor);
  }
  return cursor.value();
}

// Produces a PyFrameObject for the current shadow frame in the stack walk.
using PyFrameMaterializer = std::function<BorrowedRef<PyFrameObject>(void)>;

// Called during stack walking for each item on the call stack. Returns false
// to terminate stack walking.
using FrameHandler =
    std::function<bool(const CodeObjLoc&, PyFrameMaterializer)>;

void doShadowStackWalk(PyThreadState* tstate, FrameHandler handler) {
  BorrowedRef<PyFrameObject> prev_py_frame;
  for (_PyShadowFrame* shadow_frame = tstate->shadow_frame;
       shadow_frame != nullptr;
       shadow_frame = shadow_frame->prev) {
    _PyShadowFrame_Owner owner = _PyShadowFrame_GetOwner(shadow_frame);
    switch (owner) {
      case PYSF_INTERP: {
        BorrowedRef<PyFrameObject> py_frame =
            _PyShadowFrame_GetPyFrame(shadow_frame);
        auto materializer = [&]() { return py_frame; };
        if (!handler(CodeObjLoc(py_frame), materializer)) {
          return;
        }
        prev_py_frame = py_frame;
        break;
      }
      case PYSF_JIT: {
        UnitState unit_state = getUnitState(shadow_frame);
        // We want to materialize PyFrameObjects for all the shadow frames
        // in the unit if the handler materializes a PyFrameObject for
        // any shadow frame in the unit. For example, if we were in the
        // middle of iterating over a unit whose shadow frames looked like
        //
        //   foo <- bar <- baz
        //          ^
        //          |
        //          +-- iteration is here
        //
        // and the handler materialized a PyFrameObject for bar, then
        // we would also need to materialize the PyFrameObjects for foo
        // and baz.
        bool materialized = false;
        auto materializeUnitPyFrames = [&] {
          if (materialized) {
            return;
          }
          prev_py_frame =
              materializePyFrames(tstate, unit_state, prev_py_frame);
          materialized = true;
        };
        // Process all the frames (inlined + non-inlined) in the unit as a
        // single chunk, starting with innermost inlined frame.
        for (auto it = unit_state.rbegin(); it != unit_state.rend(); ++it) {
          shadow_frame = it->shadow_frame;
          auto materializer = [&] {
            materializeUnitPyFrames();
            return _PyShadowFrame_GetPyFrame(shadow_frame);
          };
          if (!handler(it->loc, materializer)) {
            return;
          }
        }
        break;
      }
    }
  }
}

// Invoke handler for each frame on the shadow stack
void walkShadowStack(PyThreadState* tstate, FrameHandler handler) {
  doShadowStackWalk(tstate, handler);
  if (kPyDebug) {
    assertShadowCallStackConsistent(tstate);
  }
}

// Called during stack walking for each item on the async stack. Returns false
// to terminate stack walking.
using AsyncFrameHandler =
    std::function<bool(PyObject*, const CodeObjLoc&, PyObject*)>;

// Invoke handler for each shadow frame on the async stack.
void walkAsyncShadowStack(PyThreadState* tstate, AsyncFrameHandler handler) {
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;
  while (shadow_frame != nullptr) {
    Ref<> qualname =
        Ref<>::steal(_PyShadowFrame_GetFullyQualifiedName(shadow_frame));
    _PyShadowFrame_Owner owner = _PyShadowFrame_GetOwner(shadow_frame);
    switch (owner) {
      case PYSF_INTERP: {
        PyFrameObject* py_frame = _PyShadowFrame_GetPyFrame(shadow_frame);
        if (!handler(
                qualname.get(), CodeObjLoc(py_frame), (PyObject*)py_frame)) {
          return;
        }
        break;
      }
      case PYSF_JIT: {
        // Process all the frames (inlined + non-inlined) in the unit as a
        // single chunk, starting with innermost inlined frame.
        UnitState unit_state = getUnitState(shadow_frame);
        for (auto it = unit_state.rbegin(); it != unit_state.rend(); ++it) {
          if (!handler(qualname.get(), it->loc, nullptr)) {
            return;
          }
        }
        // Set current shadow frame to the non-inlined frame.
        shadow_frame = unit_state[0].shadow_frame;
        break;
      }
    }
    _PyShadowFrame* awaiter_frame =
        _PyShadowFrame_GetAwaiterFrame(shadow_frame);
    if (awaiter_frame != nullptr) {
      shadow_frame = awaiter_frame;
    } else {
      shadow_frame = shadow_frame->prev;
    }
  }
}

const char* shadowFrameKind(_PyShadowFrame* sf) {
  switch (_PyShadowFrame_GetPtrKind(sf)) {
    case PYSF_PYFRAME:
      return "fra";
    case PYSF_CODE_RT:
      return "crt";
    case PYSF_RTFS:
      return "inl";
    case PYSF_DUMMY:
      return "<dummy>";
  }
  JIT_ABORT("Unknown shadow frame kind {}", _PyShadowFrame_GetPtrKind(sf));
}

} // namespace

Ref<PyFrameObject> materializePyFrameForDeopt(PyThreadState* tstate) {
  UnitState unit_state = getUnitState(tstate->shadow_frame);
  materializePyFrames(tstate, unit_state, nullptr);
  return Ref<PyFrameObject>::steal(tstate->frame);
}

void assertShadowCallStackConsistent(PyThreadState* tstate) {
  PyFrameObject* py_frame = tstate->frame;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;

  std::vector<_PyShadowFrame*> frames;
  while (shadow_frame) {
    frames.push_back(shadow_frame);
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      if (py_frame != _PyShadowFrame_GetPyFrame(shadow_frame)) {
        std::fprintf(stderr, "topmost:\n");
        for (size_t i = 0; i < frames.size(); i++) {
          _PyShadowFrame* sf = frames.at(i);
          Ref<> sf_name =
              Ref<>::steal(_PyShadowFrame_GetFullyQualifiedName(sf));
          const char* sf_name_str =
              sf_name == nullptr ? "<null>" : PyUnicode_AsUTF8(sf_name);
          if (sf_name_str == nullptr) {
            sf_name_str = "<null>";
          }
          std::fprintf(
              stderr,
              "  %s prev=%p data=%p name=%s\n",
              shadowFrameKind(sf),
              reinterpret_cast<void*>(shadow_frame->prev),
              reinterpret_cast<void*>(shadow_frame->data),
              sf_name_str);
        }
      }
      JIT_CHECK(
          py_frame == _PyShadowFrame_GetPyFrame(shadow_frame),
          "Inconsistent shadow and py frame ({} vs {})",
          codeName(py_frame->f_code),
          codeName(_PyShadowFrame_GetPyFrame(shadow_frame)->f_code));
      py_frame = py_frame->f_back;
    }
    shadow_frame = shadow_frame->prev;
  }

  if (py_frame != nullptr) {
    std::unordered_set<PyFrameObject*> seen;
    JIT_LOG(
        "Stack walk didn't consume entire python stack! Here's what's left:");
    PyFrameObject* left = py_frame;
    while (left && !seen.count(left)) {
      JIT_LOG("{}", PyUnicode_AsUTF8(left->f_code->co_name));
      seen.insert(left);
      left = left->f_back;
    }
    JIT_ABORT("stack walk didn't consume entire python stack");
  }
}

BorrowedRef<PyFrameObject> materializeShadowCallStack(PyThreadState* tstate) {
  walkShadowStack(
      tstate, [](const CodeObjLoc&, PyFrameMaterializer makePyFrame) {
        makePyFrame();
        return true;
      });
  return tstate->frame;
}

BorrowedRef<PyFrameObject> materializePyFrameForGen(
    PyThreadState* tstate,
    PyGenObject* gen) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  if (gen_footer->state == Ci_JITGenState_Completed) {
    return nullptr;
  }

  _PyShadowFrame* shadow_frame = &gen->gi_shadow_frame;
  UnitState unit_state = getUnitState(shadow_frame);
  // TODO(T116587512): Support inlined frames in generator objects
  JIT_CHECK(
      unit_state.size() == 1, "unexpected inlined frames found for generator");
  std::optional<BorrowedRef<PyFrameObject>> cursor;
  if (Ci_JITGenIsExecuting(gen) && !gen->gi_frame) {
    // Check if the generator's shadow frame is on the call stack. The generator
    // will be marked as running but will not be on the stack when it appears as
    // a predecessor in a chain of generators into which an exception was
    // thrown. For example, given an "await stack" of coroutines like the
    // following, where ` a <- b` indicates a `a` awaits `b`,
    //
    //   coro0 <- coro1 <- coro2
    //
    // if someone does `coro0.throw(...)`, then `coro0` and `coro1` will be
    // marked as running but will not appear on the stack while `coro2` is
    // handling the exception.
    cursor = findInnermostPyFrameForShadowFrame(tstate, shadow_frame);
  }

  return materializePyFrames(tstate, unit_state, cursor);
}

RuntimeFrameState runtimeFrameStateFromShadowFrame(
    _PyShadowFrame* shadow_frame) {
  JIT_CHECK(shadow_frame != nullptr, "Null shadow frame");
  void* shadow_ptr = _PyShadowFrame_GetPtr(shadow_frame);
  JIT_CHECK(
      shadow_ptr != nullptr,
      "Loaded a null pointer value from shadow frame {}",
      static_cast<void*>(shadow_frame));
  _PyShadowFrame_PtrKind kind = _PyShadowFrame_GetPtrKind(shadow_frame);
  switch (kind) {
    case PYSF_PYFRAME: {
      BorrowedRef<PyFrameObject> frame =
          static_cast<PyFrameObject*>(shadow_ptr);
      return RuntimeFrameState{
          frame->f_code, frame->f_builtins, frame->f_globals};
    }
    case PYSF_CODE_RT:
      return *static_cast<jit::CodeRuntime*>(shadow_ptr)->frameState();
    case PYSF_RTFS:
      return *static_cast<const jit::RuntimeFrameState*>(shadow_ptr);
    default:
      JIT_ABORT(
          "Unrecognized kind '{}' for shadow frame {}",
          kind,
          static_cast<void*>(shadow_frame));
  }
}

RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate) {
  // Get info from the shadow frame if it exists.
  if (_PyShadowFrame* shadow_frame = tstate->shadow_frame) {
    return runtimeFrameStateFromShadowFrame(shadow_frame);
  }
  PyFrameObject* frame = tstate->frame;
  JIT_CHECK(frame != nullptr, "Do not have a shadow frame or a Python frame");
  return RuntimeFrameState{frame->f_code, frame->f_builtins, frame->f_globals};
}

} // namespace jit

PyCodeObject* Ci_ShadowFrame_GetCode_JIT(_PyShadowFrame* shadow_frame) {
  return jit::runtimeFrameStateFromShadowFrame(shadow_frame).code();
}

int Ci_ShadowFrame_HasGen_JIT(_PyShadowFrame* shadow_frame) {
  return is_shadow_frame_for_gen(shadow_frame);
}

PyObject* Ci_ShadowFrame_GetModuleName_JIT(_PyShadowFrame* shadow_frame) {
  return jit::getModuleName(shadow_frame).release();
}

int Ci_ShadowFrame_WalkAndPopulate(
    PyCodeObject** async_stack,
    int* async_linenos,
    PyCodeObject** sync_stack,
    int* sync_linenos,
    int array_capacity,
    int* async_stack_len_out,
    int* sync_stack_len_out) {
  PyThreadState* tstate = PyThreadState_GET();
  // Don't assume the inputs are clean
  *async_stack_len_out = 0;
  *sync_stack_len_out = 0;

  // First walk the async stack
  jit::walkAsyncShadowStack(
      tstate, [&](PyObject*, const jit::CodeObjLoc& loc, PyObject* /*unused*/) {
        int idx = *async_stack_len_out;
        async_stack[idx] = loc.code;
        async_linenos[idx] = loc.lineNo();
        (*async_stack_len_out)++;
        return *async_stack_len_out < array_capacity;
      });

  // Next walk the sync stack
  jit::walkShadowStack(
      tstate, [&](const jit::CodeObjLoc& loc, jit::PyFrameMaterializer) {
        int idx = *sync_stack_len_out;
        sync_stack[idx] = loc.code;
        sync_linenos[idx] = loc.lineNo();
        (*sync_stack_len_out)++;
        return *sync_stack_len_out < array_capacity;
      });

  return 0;
}

void Ci_WalkStack(PyThreadState* tstate, CiWalkStackCallback cb, void* data) {
  jit::walkShadowStack(
      tstate, [&](const jit::CodeObjLoc& loc, jit::PyFrameMaterializer) {
        return cb(data, loc.code, loc.lineNo()) == CI_SWD_CONTINUE_STACK_WALK;
      });
}

void Ci_WalkAsyncStack(
    PyThreadState* tstate,
    CiWalkAsyncStackCallback cb,
    void* data) {
  jit::walkAsyncShadowStack(
      tstate,
      [&](PyObject* qualname, const jit::CodeObjLoc& loc, PyObject* pyFrame) {
        return cb(data, qualname, loc.code, loc.lineNo(), pyFrame) ==
            CI_SWD_CONTINUE_STACK_WALK;
      });
}

#else // PY_VERSION_HEX < 0x030C0000

#include "cinderx/Upgrade/upgrade_assert.h"

namespace jit {

RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate) {
  UPGRADE_ASSERT(FRAME_HANDLING_CHANGED);
}

Ref<PyFrameObject> materializePyFrameForDeopt(PyThreadState* tstate) {
  UPGRADE_ASSERT(FRAME_HANDLING_CHANGED);
}
} // namespace jit

#endif
