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

STUB(Ci_PyGCImpl*, Ci_PyGC_SetImpl, struct _gc_runtime_state *gc_state, Ci_PyGCImpl *impl)
STUB(Ci_PyGCImpl*, Ci_PyGC_GetImpl, struct _gc_runtime_state *gc_state)
STUB(void, Ci_PyGC_ClearFreeLists, PyInterpreterState *interp)
STUB(int, Ci_GenIsCompleted, PyGenObject *gen)
STUB(CiJITGenState, Ci_GetJITGenState, PyGenObject *gen)
STUB(int, Ci_GenIsExecuting, PyGenObject *gen)
STUB(int, Ci_JITGenIsExecuting, PyGenObject *gen)
STUB(PyObject*, CiCoro_New_NoFrame, PyThreadState *tstate, PyCodeObject *code)
STUB(PyObject*, CiAsyncGen_New_NoFrame, PyCodeObject *code)
STUB(PyObject*, CiGen_New_NoFrame, PyCodeObject *code)
STUB(void, _PyAwaitable_SetAwaiter, PyObject *receiver, PyObject *awaiter)
STUB(int, Ci_PyWaitHandle_CheckExact, PyObject* obj)
STUB(void, Ci_PyWaitHandle_Release, PyObject *wait_handle)
STUB(uintptr_t, _PyShadowFrame_MakeData, void *ptr, _PyShadowFrame_PtrKind ptr_kind, _PyShadowFrame_Owner owner)
STUB(void, _PyShadowFrame_SetOwner, _PyShadowFrame *shadow_frame, _PyShadowFrame_Owner owner)
STUB(void, _PyShadowFrame_Pop, PyThreadState *tstate, _PyShadowFrame *shadow_frame)
STUB(_PyShadowFrame_PtrKind, _PyShadowFrame_GetPtrKind, _PyShadowFrame *shadow_frame)
STUB(_PyShadowFrame_Owner, _PyShadowFrame_GetOwner, _PyShadowFrame *shadow_frame)
STUB(PyGenObject*, _PyShadowFrame_GetGen, _PyShadowFrame *shadow_frame)
STUB(_PyShadowFrame_PtrKind, JITShadowFrame_GetRTPtrKind, JITShadowFrame *jit_sf)
STUB(void*, JITShadowFrame_GetRTPtr, JITShadowFrame *jit_sf)
STUB(PyFrameObject*, _PyShadowFrame_GetPyFrame, _PyShadowFrame *shadow_frame)
STUB(PyCodeObject*, _PyShadowFrame_GetCode, _PyShadowFrame* shadow_frame)
STUB(PyObject*, _PyShadowFrame_GetFullyQualifiedName, _PyShadowFrame* shadow_frame)
STUB(int, Cix_eval_frame_handle_pending, PyThreadState *tstate)
STUB(PyObject*, Cix_special_lookup, PyThreadState *tstate, PyObject *o, _Py_Identifier *id)
STUB(void, Cix_format_kwargs_error, PyThreadState *tstate, PyObject *func, PyObject *kwargs)
STUB(void, Cix_format_awaitable_error, PyThreadState *tstate, PyTypeObject *type, int prevprevopcode, int prevopcode)
STUB(PyFrameObject*, Cix_PyEval_MakeFrameVector, PyThreadState *tstate, PyFrameConstructor *con, PyObject *locals, PyObject *const *args, Py_ssize_t argcount, PyObject *kwnames)
STUB(PyObject*, Cix_SuperLookupMethodOrAttr, PyThreadState *tstate, PyObject *global_super, PyTypeObject *type, PyObject *self, PyObject *name, int call_no_args, int *meth_found)
STUB(int, Cix_do_raise, PyThreadState *tstate, PyObject *exc, PyObject *cause)
STUB(void, Cix_format_exc_check_arg, PyThreadState *, PyObject *, const char *, PyObject *)
STUB(PyObject*, Cix_match_class, PyThreadState *tstate, PyObject *subject, PyObject *type, Py_ssize_t nargs, PyObject *kwargs)
STUB(PyObject*, Cix_match_keys, PyThreadState *tstate, PyObject *map, PyObject *keys)
STUB(PyObject, *Ci_Super_Lookup, PyTypeObject *type, PyObject *obj, PyObject *name, PyObject *super_instance, int *meth_found)
STUB(int, Cix_cfunction_check_kwargs, PyThreadState *tstate, PyObject *func, PyObject *kwnames)
STUB(funcptr, Cix_cfunction_enter_call, PyThreadState *tstate, PyObject *func)
STUB(funcptr, Cix_method_enter_call, PyThreadState *tstate, PyObject *func)
STUB(PyObject, * builtin_next, PyObject *self, PyObject *const *args, Py_ssize_t nargs)
STUB(PyObject, * Ci_Builtin_Next_Core, PyObject *it, PyObject *def)
STUB(PyObject*, Cix_method_get_doc, PyMethodDescrObject *descr, void *closure)
STUB(PyObject*, Cix_descr_get_qualname, PyDescrObject *descr, void *closure)
STUB(PyObject*, Cix_method_get_text_signature, PyMethodDescrObject *descr, void *closure)
STUB(PyObject*, Cix_meth_get__doc__, PyCFunctionObject *m, void *closure)
STUB(PyObject*, Cix_meth_get__name__, PyCFunctionObject *m, void *closure)
STUB(PyObject*, Cix_meth_get__qualname__, PyCFunctionObject *m, void *closure)
STUB(PyObject*, Cix_meth_get__self__, PyCFunctionObject *m, void *closure)
STUB(PyObject*, Cix_meth_get__text_signature__, PyCFunctionObject *m, void *closure)
STUB(int, _PyDict_HasUnsafeKeys, PyObject *dict)
STUB(int, _PyDict_HasOnlyUnicodeKeys, PyObject *)
STUB(Py_ssize_t, _PyDictKeys_GetSplitIndex, PyDictKeysObject *keys, PyObject *key)
STUB(uint64_t, _PyDict_NotifyEvent, PyDict_WatchEvent event, PyDictObject *mp, PyObject *key, PyObject *value)
STUB(PyObject**, Ci_PyObject_GetDictPtrAtOffset, PyObject *obj, Py_ssize_t dictoffset)
STUB(PyObject*, _PyDict_GetItem_UnicodeExact, PyObject *op, PyObject *key)
STUB(PyDictKeysObject*, _PyDict_MakeKeysShared, PyObject *op)
STUB(int, PyDict_NextKeepLazy, PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
STUB(int, Ci_set_attribute_error_context, PyObject *v, PyObject *name)
STUB(void, _PyType_ClearNoShadowingInstances, struct _typeobject *, PyObject *obj)
STUB(PyObject*, Ci_PyClassMethod_GetFunc, PyObject *method)
STUB(PyObject*, Ci_PyStaticMethod_GetFunc, PyObject *sm)
STUB(int, PyUnstable_PerfTrampoline_CompileCode, PyCodeObject *)
STUB(int, PyUnstable_PerfTrampoline_SetPersistAfterFork, int enable)
STUB(void, Ci_ThreadState_SetProfileInterpAll, int enabled)
STUB(void, Ci_RuntimeState_SetProfileInterpPeriod, long)
STUB(PyObject*, _PyObject_GC_Malloc, size_t size)
STUB(PyObject*, Ci_StaticFunction_Vectorcall, PyObject *func, PyObject* const* stack, size_t nargsf, PyObject *kwnames)
STUB(PyObject*, Ci_PyFunction_CallStatic, PyFunctionObject *func, PyObject* const* args, Py_ssize_t nargsf, PyObject *kwnames)
STUB(PyObject*, _PyObject_CallNoArg, PyObject* func)

/*
 * From checkd_dict.h
 */

STUB(PyObject*, Ci_CheckedDict_New, PyTypeObject *type)
STUB(PyObject*, Ci_CheckedDict_NewPresized, PyTypeObject *type, Py_ssize_t minused)
STUB(int, Ci_CheckedDict_Check, PyObject *x)
STUB(int, Ci_CheckedDict_TypeCheck, PyTypeObject *type)
STUB(int, Ci_CheckedDict_SetItem, PyObject *op, PyObject *key, PyObject *value)
STUB(int, Ci_DictOrChecked_SetItem, PyObject *op, PyObject *key, PyObject *value)


/*
 * From interpter.h
 */

STUB(PyObject*, Ci_GetAIter, PyThreadState *tstate, PyObject *obj)
STUB(PyObject*, Ci_GetANext, PyThreadState *tstate, PyObject *aiter)
STUB(PyObject*, Ci_EvalFrame, PyThreadState *tstate, PyFrameObject *f, int throwflag)
