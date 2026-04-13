// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/frame_shadow.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "internal/pycore_frame.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interpframe_structs.h"
#endif

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/module_state.h"

namespace jit {

RuntimeFrameState runtimeFrameStateFromThreadState(PyThreadState* tstate);

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
void jitFrameRemoveReifier(_PyInterpreterFrame* frame);

// Turns a stack allocated interpreter frame into a slab allocated one.
_PyInterpreterFrame* convertInterpreterFrameFromStackToSlab(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame);

// Gets the PyFunctionObject that is stashed away in a JIT frame. We
// store the function as an extra local.
BorrowedRef<PyFunctionObject> jitFrameGetFunction(_PyInterpreterFrame* frame);
// Sets the PyFunctionObject to be stashed away in an interpreter frame.
void jitFrameSetFunction(_PyInterpreterFrame* frame, PyFunctionObject* func);

// Checks if the interpreter frame is an inline frame w/ runtime frame state
bool hasRtfsFunction(_PyInterpreterFrame* frame);

// Get the RuntimeFrameState from a _PyInterpreterFrame, hasRtfsFunction must be
// true.
RuntimeFrameState* jitFrameGetRtfs(_PyInterpreterFrame* frame);

FrameHeader* jitFrameGetHeader(_PyInterpreterFrame* frame);

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
    _frameowner owner,
    _PyInterpreterFrame* previous,
    PyObject* reifier);

// Gets the frame size (in number of words) that's required for the JIT
// to initialize a frame object.
size_t jitFrameGetSize(PyCodeObject* code);

Ref<> makeFrameReifier(BorrowedRef<PyCodeObject> code);

} // namespace jit

#endif
