// Copyright (c) Meta Platforms, Inc. and affiliates.
#define __UPGRADE_STUBS_CPP

#include <Python.h>

#include "cinderx/Upgrade/upgrade_stubs.h"  // @donotremove
#include "cinderx/Upgrade/upgrade_unexported.h"  // @donotremove
#include "cinderx/Interpreter/interpreter.h"

#define STUB(ret, func, task, args...) ret func(args) { \
    UPGRADE_ASSERT(Hit stubbed function: func task); \
  }

extern "C" {
STUB(Ci_PyGCImpl*, Ci_PyGC_SetImpl, T196759328, struct _gc_runtime_state*, Ci_PyGCImpl*)
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
STUB(uintptr_t, _PyShadowFrame_MakeData, T194018580, void*, _PyShadowFrame_PtrKind, _PyShadowFrame_Owner)
STUB(void, _PyShadowFrame_SetOwner, T194018580, _PyShadowFrame*, _PyShadowFrame_Owner)
STUB(void, _PyShadowFrame_Pop, T194018580, PyThreadState*, _PyShadowFrame*)
STUB(_PyShadowFrame_PtrKind, _PyShadowFrame_GetPtrKind, T194018580, _PyShadowFrame*)
STUB(_PyShadowFrame_Owner, _PyShadowFrame_GetOwner, T194018580, _PyShadowFrame*)
STUB(PyGenObject*, _PyShadowFrame_GetGen, T194018580, _PyShadowFrame*)
STUB(_PyShadowFrame_PtrKind, T194018580, JITShadowFrame_GetRTPtrKind, JITShadowFrame*)
STUB(void*, JITShadowFrame_GetRTPtr, T194018580, JITShadowFrame*)
STUB(PyFrameObject*, _PyShadowFrame_GetPyFrame, T194018580, _PyShadowFrame*)
STUB(PyCodeObject*, _PyShadowFrame_GetCode, T194018580, _PyShadowFrame*)
STUB(PyObject*, _PyShadowFrame_GetFullyQualifiedName, T194018580, _PyShadowFrame* shadow_frame)
STUB(int, Cix_eval_frame_handle_pending, T196762792, PyThreadState*)
STUB(PyObject*, Cix_special_lookup, T196762792, PyThreadState*, PyObject*, _Py_Identifier*)
STUB(void, Cix_format_kwargs_error, T196762792, PyThreadState*, PyObject*, PyObject*)
STUB(void, Cix_format_awaitable_error, T196762792, PyThreadState*, PyTypeObject*, int, int)
STUB(PyFrameObject*, Cix_PyEval_MakeFrameVector, T196762792, PyThreadState*, PyFrameConstructor*, PyObject*, PyObject * const*, Py_ssize_t, PyObject*)
STUB(PyObject*, Cix_SuperLookupMethodOrAttr, T196762792, PyThreadState*, PyObject*, PyTypeObject*, PyObject*, PyObject*, int, int*)
STUB(int, Cix_do_raise, T196762792, PyThreadState*, PyObject*, PyObject*)
STUB(void, Cix_format_exc_check_arg, T196762792, PyThreadState*, PyObject*, const char*, PyObject*)
STUB(PyObject*, Cix_match_class, T196762792, PyThreadState*, PyObject*, PyObject*, Py_ssize_t, PyObject*)
STUB(PyObject*, Cix_match_keys, T196762792, PyThreadState*, PyObject*, PyObject*)
STUB(int, Cix_cfunction_check_kwargs, T196762792, PyThreadState*, PyObject*, PyObject*)
STUB(funcptr, Cix_cfunction_enter_call, T196762792, PyThreadState*, PyObject*)
STUB(funcptr, Cix_method_enter_call, T196762792, PyThreadState*, PyObject*)
STUB(PyObject*, builtin_next, T196761974, PyObject*, PyObject* const*, Py_ssize_t)
STUB(PyObject*, Ci_Builtin_Next_Core, T196761974, PyObject*, PyObject*)
// We added this and it's hard to get out of the runtime as it checks equality on a static function.
STUB(int, _PyDict_HasOnlyUnicodeKeys, T196879402, PyObject *)
STUB(Py_ssize_t, _PyDictKeys_GetSplitIndex, T196879402, PyDictKeysObject*, PyObject*)
STUB(PyObject**, Ci_PyObject_GetDictPtrAtOffset, T196879402, PyObject*, Py_ssize_t)
STUB(PyObject*, _PyDict_GetItem_Unicode, T196879402, PyObject*, PyObject*)
STUB(PyObject*, _PyDict_GetItem_UnicodeExact, T196879402, PyObject*, PyObject*)
// Only used by Strict Modules (we added it)
STUB(int, PyDict_NextKeepLazy, T196879402, PyObject*, Py_ssize_t*, PyObject**, PyObject**)

STUB(void, _PyType_ClearNoShadowingInstances, T197103405, struct _typeobject *, PyObject*)

// Needs back-porting from 3.13
STUB(int, PyUnstable_PerfTrampoline_CompileCode, T196877712, PyCodeObject *)
STUB(int, PyUnstable_PerfTrampoline_SetPersistAfterFork, T196877712, int)

// This looks like it was a Cix but has actually changed a bit in 3.12. The problem
// with using PyObject_Malloc directly is we need to know how much additional space
// to allocate for the GC info. In 3.12 it might be possible to use
// PyUnstable_Object_GC_NewWithExtraData
STUB(PyObject*, _PyObject_GC_Malloc, T???, size_t)

/*
 * From interpter.h
 */

STUB(PyObject*, Ci_GetAIter, T190615535, PyThreadState*, PyObject*)
STUB(PyObject*, Ci_GetANext, T190615535, PyThreadState*, PyObject*)
STUB(PyObject*, Ci_EvalFrame, T190615535, PyThreadState*, PyFrameObject*, int)
STUB(PyObject*, Ci_StaticFunction_Vectorcall, T190615535, PyObject *func, PyObject* const* stack, size_t nargsf, PyObject *kwnames)
STUB(PyObject*, Ci_PyFunction_CallStatic, T190615535, PyFunctionObject *func, PyObject* const* args, Py_ssize_t nargsf, PyObject *kwnames)


/*
 * From upgrade_unexported.h
 */

// These functions are all not exported so they are unavailable when libpython
// is dynamically linked. However, they are available if linking is static, as
// is the case for things like the RuntimeTime/StrictModules tests. So we define
// them weakly.

#define STUB_WEAK(ret, func, args...) ret __attribute__((weak)) func(args) { \
    UPGRADE_ASSERT(Hit stubbed function: func); \
  }

STUB_WEAK(PyObject*, _PyCoro_GetAwaitableIter, PyObject*)
STUB_WEAK(PyObject*, _PyAsyncGenValueWrapperNew, PyThreadState*, PyObject *)
STUB_WEAK(int, _PyObjectDict_SetItem, PyTypeObject*, PyObject **, PyObject*, PyObject*)
STUB_WEAK(void, _PyDictKeys_DecRef, PyDictKeysObject*)
STUB_WEAK(PyObject*, _PyDict_LoadGlobal, PyDictObject*, PyDictObject*, PyObject*)
STUB_WEAK(PyObject*, _PyTuple_FromArray, PyObject * const*, Py_ssize_t)
STUB_WEAK(static_builtin_state*, _PyStaticType_GetState, PyInterpreterState *, PyTypeObject *)
STUB_WEAK(PyObject*, _Py_union_type_or, PyObject *, PyObject *)
// We can avoid this by notifying our own dictionary watchers manually.
STUB_WEAK(void, _PyDict_SendEvent, int, PyDict_WatchEvent, PyDictObject *, PyObject *,  PyObject *)

} // extern "C"
