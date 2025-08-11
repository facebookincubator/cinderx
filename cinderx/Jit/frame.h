// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "internal/pycore_shadow_frame_struct.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"

namespace jit {

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions. Note these will be garbage in generator objects.
struct FrameHeader {
  JITShadowFrame shadow_frame;
};

// Materialize all the Python frames for the shadow stack associated with
// tstate.
//
// Returns a borrowed reference to top of the Python stack (tstate->frame).
BorrowedRef<PyFrameObject> materializeShadowCallStack(PyThreadState* tstate);

// Materialize a Python frame for the top-most frame for tstate, with the
// expectation that this frame will immediately either be unwound or resumed in
// the interpreter.
//
// NB: This returns a stolen reference to the frame. The caller is responsible
// for ensuring that the frame is unlinked and the reference is destroyed.
Ref<PyFrameObject> materializePyFrameForDeopt(PyThreadState* tstate);

// Materialize a Python frame for gen.
//
// This returns nullptr if gen is completed or a borrowed reference to its
// PyFrameObject otherwise.
BorrowedRef<PyFrameObject> materializePyFrameForGen(
    PyThreadState* tstate,
    PyGenObject* gen);

void assertShadowCallStackConsistent(PyThreadState* tstate);

// Load a runtime frame state object from a given shadow frame.
RuntimeFrameState runtimeFrameStateFromShadowFrame(
    _PyShadowFrame* shadow_frame);

// Load a runtime frame state object from a given Python thread.  Handles Python
// frames and shadow frames.
RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate);

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

} // namespace jit

extern "C" {

PyCodeObject* Ci_ShadowFrame_GetCode_JIT(_PyShadowFrame* shadow_frame);

int Ci_ShadowFrame_HasGen_JIT(_PyShadowFrame* shadow_frame);

PyObject* Ci_ShadowFrame_GetModuleName_JIT(_PyShadowFrame* shadow_frame);

int Ci_ShadowFrame_WalkAndPopulate(
    PyCodeObject** async_stack,
    int* async_linenos,
    PyCodeObject** sync_stack,
    int* sync_linenos,
    int array_capacity,
    int* async_stack_len_out,
    int* sync_stack_len_out);

// In Cinder 3.10 these are declared in cinder/exports.h
#if PY_VERSION_HEX >= 0x030C0000
void Ci_WalkAsyncStack(
    PyThreadState* tstate,
    CiWalkAsyncStackCallback cb,
    void* data);

void Ci_WalkStack(PyThreadState* tstate, CiWalkStackCallback cb, void* data);
#endif

} // extern "C"

#else // PY_VERSION_HEX < 0x030C0000

#include "internal/pycore_frame.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/module_state.h"

namespace jit {

RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate);

#ifdef ENABLE_LIGHTWEIGHT_FRAMES

// A singleton reifier object that was set _PyInterpreterFrame's f_funcobj
// to. The Python runtime will call this object when it needs a complete
// frame.
struct JitFrameReifier {
  PyObject_HEAD
  vectorcallfunc vectorcall;
};

extern PyType_Spec JitFrameReifier_Spec;

// The vectorcallfunc which gets used for our singleton JitFrameReifier object.
PyObject* jitFrameReifierVectorcall(
    JitFrameReifier* state,
    PyObject* const* args,
    size_t nargsf);

// Initializes lazily populated data in the frame. This can include the locals,
// builtins, globals, etc...
void jitFramePopulateFrame(_PyInterpreterFrame* frame);

// Initializes the frame so that it holds directly onto the function object
// removing the reifier object. This is required before returning execution
// to the interpreter.
void jitFrameInitFunctionObject(_PyInterpreterFrame* frame);

// Turns a stack allocated interpreter frame into a slab allocated one.
_PyInterpreterFrame* convertInterpreterFrameFromStackToSlab(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame);

// Gets the PyFunctionObject that is stashed away in a JIT frame. We
// store the function as an extra local.
BorrowedRef<PyFunctionObject> jitFrameGetFunction(_PyInterpreterFrame* frame);
// Sets the PyFunctionObject to be stashed away in an interpreter frame.
void jitFrameSetFunction(_PyInterpreterFrame* frame, PyFunctionObject* func);

#endif

// Like _PyFrame_ClearExceptCode but will handle partially initialized
// JIT frames and only clean up the necessary state.
void jitFrameClearExceptCode(_PyInterpreterFrame* frame);

// Initializes a JIT interpreter frame. Equivalent to _PyFrame_Initialize if we
// don't have ENABLE_LIGHTWEIGHT_FRAMES. If we do have ENABLE_LIGHTWEIGHT_FRAMES
// then this will only initialize the subset of the fields which are
// required.
void jitFrameInit(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyFunctionObject* func,
    PyCodeObject* code,
    int null_locals_from,
    _PyInterpreterFrame* previous,
    bool generator);

// Checks if the interpreter frame is an inline frame w/ runtime frame state
bool hasRtfsFunction(_PyInterpreterFrame* frame);

// Get the RuntimeFrameState from a _PyInterpreterFrame, hasRtfsFunction must be
// true.
RuntimeFrameState* jitFrameGetRtfs(_PyInterpreterFrame* frame);

// Gets the frame size (in number of words) that's required for the JIT
// to initialize a frame object.
size_t jitFrameGetSize(PyCodeObject* code);

int frameHeaderSize(BorrowedRef<PyCodeObject> code);

#define JIT_FRAME_RTFS 0x01
#define JIT_FRAME_INITIALIZED 0x02
#define JIT_FRAME_MASK 0x03

// FrameHeader lives at the beginning of the stack frame for JIT-compiled
// functions. In 3.12+ this will be followed by the _PyInterpreterFrame.
struct FrameHeader {
  union {
    PyFunctionObject* func;
    uintptr_t rtfs;
  };
};

FrameHeader* jitFrameGetHeader(_PyInterpreterFrame* frame);

} // namespace jit

#endif
