// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/compile.h"
#include "cinderx/Jit/entry.h"
#include "cinderx/Jit/pyjit_result.h"
#include "cinderx/Jit/pyjit_typeslots.h"

#include <Python.h>

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
PyAPI_FUNC(int) _PyJIT_Initialize(void);

/*
 * Enable the global JIT.
 *
 * _PyJIT_Initialize must be called before calling this.
 *
 * Returns 1 if the JIT is enabled and 0 otherwise.
 */
PyAPI_FUNC(int) _PyJIT_Enable(void);

/*
 * Disable the global JIT.
 */
PyAPI_FUNC(void) _PyJIT_Disable(void);

/*
 * Returns 1 if JIT compilation is enabled and 0 otherwise.
 */
PyAPI_FUNC(int) _PyJIT_IsEnabled(void);

/*
 * Returns 1 if auto-JIT is enabled and 0 otherwise.
 */
PyAPI_FUNC(int) _PyJIT_IsAutoJITEnabled(void);

/*
 * Get the number of calls needed to mark a function for compilation by AutoJIT.
 * Returns 0 when AutoJIT is disabled.
 */
PyAPI_FUNC(unsigned) _PyJIT_AutoJITThreshold(void);

/*
 * Informs the JIT that a type, function, or code object is being created,
 * modified, or destroyed.
 */
PyAPI_FUNC(void) _PyJIT_TypeModified(PyTypeObject* type);
PyAPI_FUNC(void) _PyJIT_TypeNameModified(PyTypeObject* type);
PyAPI_FUNC(void) _PyJIT_TypeDestroyed(PyTypeObject* type);
PyAPI_FUNC(void) _PyJIT_FuncModified(PyFunctionObject* func);
PyAPI_FUNC(void) _PyJIT_FuncDestroyed(PyFunctionObject* func);
PyAPI_FUNC(void) _PyJIT_CodeDestroyed(PyCodeObject* code);

/*
 * Clean up any resources allocated by the JIT.
 *
 * This is intended to be called at interpreter shutdown in Py_Finalize.
 *
 * Returns 0 on success or -1 on error.
 */
PyAPI_FUNC(int) _PyJIT_Finalize(void);

/* Dict-watching callbacks, invoked by dictobject.c when appropriate. */

/*
 * Send into/resume a suspended JIT generator and return the result.
 */
PyAPI_FUNC(PyObject*) _PyJIT_GenSend(
    PyGenObject* gen,
    PyObject* arg,
    int exc,
    PyFrameObject* f,
    PyThreadState* tstate,
    int finish_yield_from);

/*
 * Materialize the frame for gen. Returns a borrowed reference.
 */
PyAPI_FUNC(PyFrameObject*) _PyJIT_GenMaterializeFrame(PyGenObject* gen);

/*
 * Visit owned references in a JIT-backed generator object.
 */
PyAPI_FUNC(int)
    _PyJIT_GenVisitRefs(PyGenObject* gen, visitproc visit, void* arg);

/*
 * Release any JIT-related data in a PyGenObject.
 */
PyAPI_FUNC(void) _PyJIT_GenDealloc(PyGenObject* gen);

/*
 * Return current sub-iterator from JIT generator or NULL if there is none.
 */
PyAPI_FUNC(PyObject*) _PyJIT_GenYieldFromValue(PyGenObject* gen);

#if PY_VERSION_HEX < 0x030C0000
/*
 * Returns a borrowed reference to the globals for the top-most Python function
 * associated with tstate.
 */
PyAPI_FUNC(PyObject*) _PyJIT_GetGlobals(PyThreadState* tstate);

/*
 * Returns a borrowed reference to the builtins for the top-most Python function
 * associated with tstate.
 */
PyAPI_FUNC(PyObject*) _PyJIT_GetBuiltins(PyThreadState* tstate);
#endif

/*
 * Returns a borrowed reference to the top-most frame of tstate.
 *
 * When shadow frame mode is active, calling this function will materialize
 * PyFrameObjects for any jitted functions on the call stack.
 */
PyAPI_FUNC(PyFrameObject*) _PyJIT_GetFrame(PyThreadState* tstate);

/*
 * Set output format for function disassembly. E.g. with -X jit-disas-funcs.
 */
PyAPI_FUNC(void) _PyJIT_SetDisassemblySyntaxATT(void);
PyAPI_FUNC(int) _PyJIT_IsDisassemblySyntaxIntel(void);

PyAPI_FUNC(int) _PyPerfTrampoline_IsPreforkCompilationEnabled(void);
PyAPI_FUNC(void) _PyPerfTrampoline_CompilePerfTrampolinePreFork(void);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace jit {

/*
 * JIT compile func or code object, only if a preloader is available.
 *
 * Re-entrant compile that is safe to call from within compilation, because it
 * will only use an already-created preloader, it will not preload, and
 * therefore it cannot raise a Python exception.
 *
 * Returns PYJIT_RESULT_NO_PRELOADER if no preloader is available.
 */
_PyJIT_Result tryCompilePreloaded(BorrowedRef<> unit);

/*
 * Load the preloader for a given function or code object, if it exists.
 */
hir::Preloader* lookupPreloader(BorrowedRef<> unit);

/*
 * Check if a function or code object has been preloaded.
 */
bool isPreloaded(BorrowedRef<> unit);

/*
 * Preload given function and its compilation dependencies.
 *
 * Dependencies are functions that this function statically invokes (so we want
 * to ensure they are compiled first so we can emit a direct x64 call), and any
 * functions we can detect that this function may call, so they can potentially
 * be inlined. Exposed for test use.
 *
 */
bool preloadFuncAndDeps(BorrowedRef<PyFunctionObject> func);

using PreloaderMap = std::
    unordered_map<BorrowedRef<PyCodeObject>, std::unique_ptr<hir::Preloader>>;

/*
 * RAII device for isolating preloaders state. Exposed for test use.
 */
class IsolatedPreloaders {
 public:
  IsolatedPreloaders();
  ~IsolatedPreloaders();

 private:
  PreloaderMap orig_preloaders_;
};

} // namespace jit
#endif

#endif /* Py_LIMITED_API */
