// Copyright (c) Meta Platforms, Inc. and affiliates.
#define __UPGRADE_STUBS_CPP

#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#include <Python.h>

#include "cinderx/Interpreter/interpreter.h"

#define STUB(ret, func, task, args...)                \
  ret func(args) {                                    \
    UPGRADE_ASSERT(Hit stubbed function : func task); \
  }

extern "C" {
STUB(
    Ci_PyGCImpl*,
    Ci_PyGC_SetImpl,
    T196759328,
    struct _gc_runtime_state*,
    Ci_PyGCImpl*)
STUB(Ci_PyGCImpl*, Ci_PyGC_GetImpl, T196759328, struct _gc_runtime_state*)
STUB(void, Ci_PyGC_ClearFreeLists, T196759328, PyInterpreterState*)
STUB(int, Ci_GenIsCompleted, T194022335, PyGenObject*)
STUB(CiJITGenState, Ci_GetJITGenState, T194022335, PyGenObject*)
STUB(int, Ci_GenIsExecuting, T194022335, PyGenObject*)
STUB(int, Ci_JITGenIsExecuting, T194022335, PyGenObject*)
STUB(PyObject*, CiCoro_New_NoFrame, T194022335, PyThreadState*, PyCodeObject*)
STUB(PyObject*, CiAsyncGen_New_NoFrame, T194022335, PyCodeObject*)
STUB(PyObject*, CiGen_New_NoFrame, T194022335, PyCodeObject*)
STUB(int, Ci_PyWaitHandle_CheckExact, T194027914, PyObject*)
STUB(void, Ci_PyWaitHandle_Release, T194027914, PyObject*)
STUB(
    int,
    Cix_cfunction_check_kwargs,
    T196762792,
    PyThreadState*,
    PyObject*,
    PyObject*)
STUB(funcptr, Cix_cfunction_enter_call, T196762792, PyThreadState*, PyObject*)
STUB(funcptr, Cix_method_enter_call, T196762792, PyThreadState*, PyObject*)
STUB(
    PyObject*,
    builtin_next,
    T196761974,
    PyObject*,
    PyObject* const*,
    Py_ssize_t)
STUB(PyObject*, Ci_Builtin_Next_Core, T196761974, PyObject*, PyObject*)
// We added this and it's hard to get out of the runtime as it checks equality
// on a static function.
STUB(
    Py_ssize_t,
    _PyDictKeys_GetSplitIndex,
    T196879402,
    PyDictKeysObject*,
    PyObject*)
STUB(PyObject*, _PyDict_GetItem_Unicode, T196879402, PyObject*, PyObject*)
STUB(PyObject*, _PyDict_GetItem_UnicodeExact, T196879402, PyObject*, PyObject*)
// Only used by Strict Modules (we added it)
STUB(
    int,
    PyDict_NextKeepLazy,
    T196879402,
    PyObject*,
    Py_ssize_t*,
    PyObject**,
    PyObject**)

STUB(
    void,
    _PyType_ClearNoShadowingInstances,
    T197103405,
    struct _typeobject*,
    PyObject*)

// Needs back-porting from 3.13
STUB(int, PyUnstable_PerfTrampoline_CompileCode, T196877712, PyCodeObject*)
STUB(int, PyUnstable_PerfTrampoline_SetPersistAfterFork, T196877712, int)

/*
 * From interpter.h
 */

STUB(
    PyObject*,
    Ci_StaticFunction_Vectorcall,
    T190615535,
    PyObject* func,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames)
STUB(
    PyObject*,
    Ci_PyFunction_CallStatic,
    T190615535,
    PyFunctionObject* func,
    PyObject* const* args,
    Py_ssize_t nargsf,
    PyObject* kwnames)

} // extern "C"
