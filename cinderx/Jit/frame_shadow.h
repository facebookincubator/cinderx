// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000

#include "internal/pycore_shadow_frame_struct.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"

namespace jit {

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

inline Ref<> makeFrameReifier(BorrowedRef<PyCodeObject> code) {
  // Just for reducing ifdef's for 3.14+ support
  return nullptr;
}

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

} // extern "C"

#endif
