// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit_result.h"

namespace jit {

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
 * Returns 0 on success, -1 on error, or -2 if we just printed the JIT args.
 */
int initialize();

/*
 * Clean up any resources allocated by the JIT.
 *
 * This is intended to be called at interpreter shutdown in Py_Finalize().
 */
void finalize();

/*
 * Check if a function should be scheduled for compilation.
 *
 * This doesn't guarantee that the function can or will be compiled, it just
 * checks if the JIT has been configured in such a way that compilation is
 * possible.
 */
bool shouldScheduleCompile(BorrowedRef<PyFunctionObject> func);

/*
 * Variant of shouldScheduleCompile() for nested code objects.
 */
bool shouldScheduleCompile(
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code);

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
bool scheduleJitCompile(BorrowedRef<PyFunctionObject> func);

/*
 * JIT compile func and patch its entry point.
 *
 * On success, positional only calls to func will use the JIT compiled version.
 *
 * Returns PYJIT_RESULT_OK on success.
 */
_PyJIT_Result compileFunction(BorrowedRef<PyFunctionObject> func);

/*
 * Preload a function, along with any functions that it calls that we might want
 * to compile afterwards as well.  This is to support inlining and faster
 * invokes for Static Python functions.
 *
 * Setting the `forcePreload` will bypass the "might want to compile" logic and
 * force all the preloads to happen unconditionally.
 *
 * Return a list of preloaders that were created.  There should be at least one
 * preloader in the list, if it's empty then there was a preloading failure.
 */
std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func,
    bool forcePreload = false);

/*
 * Inform the JIT that a code, function, or type object is being modified or
 * destroyed.
 */
void codeDestroyed(BorrowedRef<PyCodeObject> code);
void funcDestroyed(BorrowedRef<PyFunctionObject> func);
void funcModified(BorrowedRef<PyFunctionObject> func);
void typeDestroyed(BorrowedRef<PyTypeObject> type);
void typeModified(BorrowedRef<PyTypeObject> type);
void typeNameModified(BorrowedRef<PyTypeObject> type);

// Exposed for unit tests
Context::CompilationResult compilePreloaderImpl(
    jit::CompilerContext<Compiler>* jit_ctx,
    const hir::Preloader& preloader);

} // namespace jit

#ifdef __cplusplus
extern "C" {
#endif

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
