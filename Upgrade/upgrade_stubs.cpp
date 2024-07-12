// Copyright (c) Meta Platforms, Inc. and affiliates.
#define __UPGRADE_STUBS_CPP

#include <Python.h>

#include "cinderx/Upgrade/upgrade_stubs.h"  // @donotremove
#include "cinderx/Upgrade/upgrade_unexported.h"  // @donotremove
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/Interpreter/interpreter.h"

#define STUB(ret, func, args...) ret func(args) { \
    UPGRADE_ASSERT(Hit stubbed function: func); \
  }

STUB(Ci_PyGCImpl*, Ci_PyGC_SetImpl, struct _gc_runtime_state*, Ci_PyGCImpl*)
STUB(Ci_PyGCImpl*, Ci_PyGC_GetImpl, struct _gc_runtime_state*)
STUB(void, Ci_PyGC_ClearFreeLists, PyInterpreterState*)
STUB(int, Ci_GenIsCompleted, PyGenObject*)
STUB(CiJITGenState, Ci_GetJITGenState, PyGenObject*)
STUB(int, Ci_GenIsExecuting, PyGenObject*)
STUB(int, Ci_JITGenIsExecuting, PyGenObject*)
STUB(PyObject*, CiCoro_New_NoFrame, PyThreadState*, PyCodeObject*)
STUB(PyObject*, CiAsyncGen_New_NoFrame, PyCodeObject*)
STUB(PyObject*, CiGen_New_NoFrame, PyCodeObject*)
STUB(void, _PyAwaitable_SetAwaiter, PyObject*, PyObject*)
STUB(int, Ci_PyWaitHandle_CheckExact, PyObject*)
STUB(void, Ci_PyWaitHandle_Release, PyObject*)
STUB(uintptr_t, _PyShadowFrame_MakeData, void*, _PyShadowFrame_PtrKind, _PyShadowFrame_Owner)
STUB(void, _PyShadowFrame_SetOwner, _PyShadowFrame*, _PyShadowFrame_Owner)
STUB(void, _PyShadowFrame_Pop, PyThreadState*, _PyShadowFrame*)
STUB(_PyShadowFrame_PtrKind, _PyShadowFrame_GetPtrKind, _PyShadowFrame*)
STUB(_PyShadowFrame_Owner, _PyShadowFrame_GetOwner, _PyShadowFrame*)
STUB(PyGenObject*, _PyShadowFrame_GetGen, _PyShadowFrame*)
STUB(_PyShadowFrame_PtrKind, JITShadowFrame_GetRTPtrKind, JITShadowFrame*)
STUB(void*, JITShadowFrame_GetRTPtr, JITShadowFrame*)
STUB(PyFrameObject*, _PyShadowFrame_GetPyFrame, _PyShadowFrame*)
STUB(PyCodeObject*, _PyShadowFrame_GetCode, _PyShadowFrame*)
STUB(PyObject*, _PyShadowFrame_GetFullyQualifiedName, _PyShadowFrame* shadow_frame)
STUB(int, Cix_eval_frame_handle_pending, PyThreadState*)
STUB(PyObject*, Cix_special_lookup, PyThreadState*, PyObject*, _Py_Identifier*)
STUB(void, Cix_format_kwargs_error, PyThreadState*, PyObject*, PyObject*)
STUB(void, Cix_format_awaitable_error, PyThreadState*, PyTypeObject*, int, int)
STUB(PyFrameObject*, Cix_PyEval_MakeFrameVector, PyThreadState*, PyFrameConstructor*, PyObject*, PyObject**, Py_ssize_t, PyObject*)
STUB(PyObject*, Cix_SuperLookupMethodOrAttr, PyThreadState*, PyObject*, PyTypeObject*, PyObject*, PyObject*, int, int*)
STUB(int, Cix_do_raise, PyThreadState*, PyObject*, PyObject*)
STUB(void, Cix_format_exc_check_arg, PyThreadState*, PyObject*, const char*, PyObject*)
STUB(PyObject*, Cix_match_class, PyThreadState*, PyObject*, PyObject*, Py_ssize_t, PyObject*)
STUB(PyObject*, Cix_match_keys, PyThreadState*, PyObject*, PyObject*)
STUB(PyObject*, Ci_Super_Lookup, PyTypeObject*, PyObject*, PyObject*, PyObject*, int*)
STUB(int, Cix_cfunction_check_kwargs, PyThreadState*, PyObject*, PyObject*)
STUB(funcptr, Cix_cfunction_enter_call, PyThreadState*, PyObject*)
STUB(funcptr, Cix_method_enter_call, PyThreadState*, PyObject*)
STUB(PyObject*, builtin_next, PyObject*, PyObject* const*, Py_ssize_t)
STUB(PyObject*, Ci_Builtin_Next_Core, PyObject*, PyObject*)
STUB(PyObject*, Cix_method_get_doc, PyMethodDescrObject*, void*)
STUB(PyObject*, Cix_descr_get_qualname, PyDescrObject*, void*)
STUB(PyObject*, Cix_method_get_text_signature, PyMethodDescrObject*, void*)
STUB(PyObject*, Cix_meth_get__doc__, PyCFunctionObject*, void*)
STUB(PyObject*, Cix_meth_get__name__, PyCFunctionObject*, void*)
STUB(PyObject*, Cix_meth_get__qualname__, PyCFunctionObject*, void*)
STUB(PyObject*, Cix_meth_get__self__, PyCFunctionObject*, void*)
STUB(PyObject*, Cix_meth_get__text_signature__, PyCFunctionObject*, void*)
STUB(int, _PyDict_HasUnsafeKeys, PyObject*)
STUB(int, _PyDict_HasOnlyUnicodeKeys, PyObject *)
STUB(Py_ssize_t, _PyDictKeys_GetSplitIndex, PyDictKeysObject*, PyObject*)
STUB(PyObject**, Ci_PyObject_GetDictPtrAtOffset, PyObject*, Py_ssize_t)
STUB(PyObject*, _PyDict_GetItem_Unicode, PyObject*, PyObject*)
STUB(PyObject*, _PyDict_GetItem_UnicodeExact, PyObject*, PyObject*)
STUB(PyDictKeysObject*, _PyDict_MakeKeysShared, PyObject*)
STUB(int, PyDict_NextKeepLazy, PyObject*, Py_ssize_t*, PyObject**, PyObject**)
STUB(int, Ci_set_attribute_error_context, PyObject*, PyObject*)
STUB(void, _PyType_ClearNoShadowingInstances, struct _typeobject *, PyObject*)
STUB(PyObject*, Ci_PyClassMethod_GetFunc, PyObject*)
STUB(PyObject*, Ci_PyStaticMethod_GetFunc, PyObject*)
STUB(int, PyUnstable_PerfTrampoline_CompileCode, PyCodeObject *)
STUB(int, PyUnstable_PerfTrampoline_SetPersistAfterFork, int)
STUB(void, Ci_ThreadState_SetProfileInterpAll, int)
STUB(void, Ci_RuntimeState_SetProfileInterpPeriod, long)
STUB(PyObject*, _PyObject_GC_Malloc, size_t)
STUB(PyObject*, Ci_StaticFunction_Vectorcall, PyObject*, PyObject* const*, size_t, PyObject*)
STUB(PyObject*, Ci_PyFunction_CallStatic, PyFunctionObject*, PyObject* const*, Py_ssize_t, PyObject*)

/*
 * From checkd_dict.h
 */

STUB(PyObject*, Ci_CheckedDict_New, PyTypeObject*)
STUB(PyObject*, Ci_CheckedDict_NewPresized, PyTypeObject*, Py_ssize_t)
STUB(int, Ci_CheckedDict_Check, PyObject*)
STUB(int, Ci_CheckedDict_TypeCheck, PyTypeObject*)
STUB(int, Ci_CheckedDict_SetItem, PyObject*, PyObject*, PyObject*)
STUB(int, Ci_DictOrChecked_SetItem, PyObject*, PyObject*, PyObject*)


/*
 * From interpter.h
 */

STUB(PyObject*, Ci_GetAIter, PyThreadState*, PyObject*)
STUB(PyObject*, Ci_GetANext, PyThreadState*, PyObject*)
STUB(PyObject*, Ci_EvalFrame, PyThreadState*, PyFrameObject*, int)


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

STUB_WEAK(PyObject*, _PyGen_yf, PyGenObject*)
STUB_WEAK(PyObject*, _PyCoro_GetAwaitableIter, PyObject*)
STUB_WEAK(PyObject*, _PyAsyncGenValueWrapperNew, PyThreadState*, PyObject *)
STUB_WEAK(int, _PyObjectDict_SetItem, PyTypeObject*, PyObject **, PyObject*, PyObject*)
STUB_WEAK(void, _PyDictKeys_DecRef, PyDictKeysObject*)
STUB_WEAK(PyObject*, _PyDict_LoadGlobal, PyDictObject*, PyDictObject*, PyObject*)
STUB_WEAK(PyObject*, _PyTuple_FromArray, PyObject* *, Py_ssize_t)
STUB_WEAK(static_builtin_state*, _PyStaticType_GetState, PyInterpreterState *, PyTypeObject *)
STUB_WEAK(PyObject*, _Py_union_type_or, PyObject *, PyObject *)
