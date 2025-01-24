// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Jit/pyjit_result.h"

#ifdef __cplusplus
#include "cinderx/Jit/hir/preload.h"
#endif

#ifndef Py_LIMITED_API
#ifdef __cplusplus
extern "C" {
#endif

/*
 * This defines the global public API for the JIT that is consumed by the
 * runtime.
 *
 * These methods assume that the GIL is held unless it is explicitly stated
 * otherwise.
 */

/*
 * Initialize any global state required by the JIT.
 *
 * This must be called before attempting to use the JIT.
 *
 * Returns 0 on success, -1 on error, or -2 if we just printed the jit args.
 */
int _PyJIT_Initialize(void);

/*
 * Clean up any resources allocated by the JIT.
 *
 * This is intended to be called at interpreter shutdown in Py_Finalize.
 *
 * Returns 0 on success or -1 on error.
 */
int _PyJIT_Finalize(void);

/*
 * JIT compile func and patch its entry point.
 *
 * On success, positional only calls to func will use the JIT compiled version.
 *
 * Returns PYJIT_RESULT_OK on success.
 */
_PyJIT_Result _PyJIT_CompileFunction(PyFunctionObject* func);

/*
 * Registers a function with the JIT to be compiled in the future.
 *
 * The JIT will still be informed by _PyJIT_CompileFunction before the function
 * executes for the first time.  The JIT can choose to compile the function at
 * some future point.
 *
 * The JIT will not keep the function alive, instead it will be informed that
 * the function is being de-allocated via _PyJIT_FuncDestroyed() before the
 * function goes away.
 *
 * Returns 1 if the function is registered with JIT or is already compiled, and
 * 0 otherwise.
 */
int _PyJIT_RegisterFunction(PyFunctionObject* func);

/*
 * Informs the JIT that a type, function, or code object is being created,
 * modified, or destroyed.
 */
void _PyJIT_CodeDestroyed(PyCodeObject* code);
void _PyJIT_FuncDestroyed(PyFunctionObject* func);
void _PyJIT_FuncModified(PyFunctionObject* func);
void _PyJIT_TypeDestroyed(PyTypeObject* type);
void _PyJIT_TypeModified(PyTypeObject* type);
void _PyJIT_TypeNameModified(PyTypeObject* type);

#if PY_VERSION_HEX < 0x030C0000
/*
 * Send into/resume a suspended JIT generator and return the result.
 */
PyObject* _PyJIT_GenSend(
    PyGenObject* gen,
    PyObject* arg,
    int exc,
    PyFrameObject* f,
    PyThreadState* tstate,
    int finish_yield_from);

/*
 * Materialize the frame for gen. Returns a borrowed reference.
 */
PyFrameObject* _PyJIT_GenMaterializeFrame(PyGenObject* gen);

/*
 * Visit owned references in a JIT-backed generator object.
 */
int _PyJIT_GenVisitRefs(PyGenObject* gen, visitproc visit, void* arg);

/*
 * Release any JIT-related data in a PyGenObject.
 */
void _PyJIT_GenDealloc(PyGenObject* gen);

/*
 * Return current sub-iterator from JIT generator or NULL if there is none.
 */
PyObject* _PyJIT_GenYieldFromValue(PyGenObject* gen);

/*
 * Returns a borrowed reference to the globals for the top-most Python function
 * associated with tstate.
 */
PyObject* _PyJIT_GetGlobals(PyThreadState* tstate);

/*
 * Returns a borrowed reference to the builtins for the top-most Python function
 * associated with tstate.
 */
PyObject* _PyJIT_GetBuiltins(PyThreadState* tstate);

/*
 * Returns a borrowed reference to the top-most frame of tstate.
 *
 * When shadow frame mode is active, calling this function will materialize
 * PyFrameObjects for any jitted functions on the call stack.
 */
PyFrameObject* _PyJIT_GetFrame(PyThreadState* tstate);
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace jit {

/*
 * Overwrite the entry point of a function so that it tries to JIT-compile
 * itself in the future.
 *
 * By default this will trigger the JIT the next time the function is called,
 * unless AutoJIT is enabled, in that case the function will compile after it is
 * called more times than the AutoJIT threshold.  Before that it will run
 * through the interpreter.
 *
 * Return true if the function was successfully scheduled for compilation, or if
 * it is already compiled.
 */
bool scheduleJitCompile(PyFunctionObject* func);

/*
 * Preload a function, along with any functions that it calls that we might want
 * to compile afterwards as well.  This is to support inlining and faster
 * invokes for Static Python functions.
 *
 * Return a list of preloaders that were created.  There should be at least one
 * preloader in the list, if it's empty then there was a preloading failure.
 */
std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func);

} // namespace jit
#endif

#endif /* Py_LIMITED_API */
