// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030E0000
// These symbols are not exported from CPython, but they are also not marked
// static. When we statically link CinderX + the runtime (which happens at least
// our RuntimeTests, and potentially with native python) we end up with
// duplicate symbols. Therefore anything that falls into this category needs to
// be renamed for it's definition in CinderX.
// Some of these also get included before we include borrowed.h, in which case
// we also need function prototypes for them.
#define _PyFrame_ClearExceptCode _CiFrame_ClearExceptCode
#define _PyObject_HasLen _CiPyObject_HasLen
#define _PyFrame_ClearLocals _CiFrame_ClearLocals

#define _PyStaticType_GetState Cix_PyStaticType_GetState
#define _PyObject_VirtualAlloc _CiVirtualAlloc
#define _PyThreadState_PushFrame _CiThreadState_PushFrame
#define _PyErr_GetTopmostException _CiErr_GetTopmostException
#define _PyEval_Vector _CiEval_Vector
#define _PyType_CacheGetItemForSpecialization \
  _CiType_CacheGetItemForSpecialization
#define _PyType_CacheInitForSpecialization _CiType_CacheInitForSpecialization
#define _PyType_LookupRefAndVersion _CiType_LookupRefAndVersion
#define _PyBuildSlice_ConsumeRefs _CiBuildSlice_ConsumeRefs
#define _PyFunction_GetVersionForCurrentState \
  _CiFunction_GetVersionForCurrentState
#define _PyExc_CreateExceptionGroup _CiExc_CreateExceptionGroup
// PyObject* _PyExc_CreateExceptionGroup(const char* msg_str, PyObject* excs);

#define _PyFloat_FromDouble_ConsumeInputs _CiFloat_FromDouble_ConsumeInputs
#define _PyDict_GetKeysVersionForCurrentState \
  _CiDict_GetKeysVersionForCurrentState
#define _PyDict_LookupIndex _CiDict_LookupIndex
#define _Py_dict_lookup _Ci_dict_lookup
#define _PyDictKeys_StringLookupSplit _CiDictKeys_StringLookupSplit
#define _PyDictKeys_GetVersionForCurrentState \
  _CiDictKeys_GetVersionForCurrentState
#define _PyDictKeys_StringLookupAndVersion _CiDictKeys_StringLookupAndVersion
#define _PyDictKeys_StringLookup _CiDictKeys_StringLookup
#define _PyStack_UnpackDict_Free _CiStack_UnpackDict_Free
#if PY_VERSION_HEX < 0x030F0000
#define _PyStack_UnpackDict_FreeNoDecRef _CiStack_UnpackDict_FreeNoDecRef
#define _PyStack_UnpackDict _CiStack_UnpackDict
#define _PyCode_InitAddressRange _CiCode_InitAddressRange
#define _PyLineTable_NextAddressRange _CiLineTable_NextAddressRange
#define _Py_Instrument _Ci_Instrument
#define _Py_call_instrumentation _Ci_call_instrumentation
#define _Py_call_instrumentation_arg _Ci_call_instrumentation_arg
#define _Py_call_instrumentation_2args _Ci_call_instrumentation_2args
#define _Py_call_instrumentation_jump _Ci_call_instrumentation_jump
#define _Py_call_instrumentation_instruction \
  _Ci_call_instrumentation_instruction
#define _Py_Instrumentation_GetLine _Ci_Instrumentation_GetLine
#define _Py_call_instrumentation_line _Ci_call_instrumentation_line
#define _Py_call_instrumentation_exc2 _Ci_call_instrumentation_exc2
#endif
#define _PyNumber_InPlacePowerNoMod _CiNumber_InPlacePowerNoMod
#define _PyNumber_PowerNoMod _CiNumber_PowerNoMod

#define _PyErr_SetObject _CiErr_SetObject

#define _PyInstrumentation_MISSING (*Cix_monitoring_missing)
#define _PyInstrumentation_DISABLE (*Cix_monitoring_disable)

#define _PyFrame_MakeAndSetFrameObject _CiFrame_MakeAndSetFrameObject
#define _PyInstruction_GetLength _CiInstruction_GetLength
#define _Py_GetBaseCodeUnit _Ci_GetBaseCodeUnit

#define _Py_Specialize_ContainsOp _Ci_Specialize_ContainsOp
#define _Py_Specialize_ToBool _Ci_Specialize_ToBool
#define _Py_Specialize_Send _Ci_Specialize_Send
#define _Py_Specialize_ForIter _Ci_Specialize_ForIter
#define _Py_Specialize_UnpackSequence _Ci_Specialize_UnpackSequence
#define _Py_Specialize_CompareOp _Ci_Specialize_CompareOp
#define _Py_Specialize_BinaryOp _Ci_Specialize_BinaryOp
#define _Py_Specialize_CallKw _Ci_Specialize_CallKw
#define _Py_Specialize_CallFunctionEx _Ci_Specialize_CallFunctionEx
#define _Py_Specialize_StoreSubscr _Ci_Specialize_StoreSubscr
#define _Py_Specialize_LoadGlobal _Ci_Specialize_LoadGlobal
#define _Py_Specialize_StoreAttr _Ci_Specialize_StoreAttr
#define _Py_Specialize_LoadAttr _Ci_Specialize_LoadAttr
#define _Py_Specialize_LoadSuperAttr _Ci_Specialize_LoadSuperAttr
#if PY_VERSION_HEX < 0x030F0000
#define _PyEval_MonitorRaise _CiEval_MonitorRaise
#define _PyEval_FrameClearAndPop _CiEval_FrameClearAndPop
#define _PyEvalFramePushAndInit _CiEvalFramePushAndInit
#define _PyEvalFramePushAndInit_Ex _CiEvalFramePushAndInit_Ex
#endif
#define _PyType_Validate _CiType_Validate

#if PY_VERSION_HEX < 0x030F0000
#define _Py_Specialize_Call _Ci_Specialize_Call
#endif

#define _PyTraceBack_FromFrame _CiTraceBack_FromFrame
#define _Py_CalculateSuggestions _Ci_CalculateSuggestions
#if PY_VERSION_HEX >= 0x030F0000
#define Cix_PyObjectDict_SetItem _PyObjectDict_SetItem
#else
#define _PyObjectDict_SetItem Cix_PyObjectDict_SetItem
#endif

#define _PyDict_CheckConsistency _CiDict_CheckConsistency
#define _Py_dict_lookup_keep_lazy _Ci_dict_lookup_keep_lazy
#define _PyDict_SetItem_LockHeld _CiDict_SetItem_LockHeld
#define _PyDict_DelItem_KnownHash_LockHeld _CiDict_DelItem_KnownHash_LockHeld
#define _PyInterpreterState_GetConfig _CiInterpreterState_GetConfig

#ifdef __cplusplus
extern "C" {
#endif
PyObject* _PyNumber_InPlacePowerNoMod(PyObject* lhs, PyObject* rhs);
PyObject* _PyNumber_PowerNoMod(PyObject* lhs, PyObject* rhs);
#ifdef __cplusplus
}
#endif

#endif

#if PY_VERSION_HEX >= 0x030D0000
// _PyTuple_MaybeUntrack stopped being exported in 3.13 but is still extern
#define _PyTuple_MaybeUntrack _CiTuple_MaybeUntrack
#if PY_VERSION_HEX >= 0x030F0000
void _PyTuple_MaybeUntrack(PyObject*);
#endif
#endif

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_dict.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_typeobject.h"

#include "cinderx/python_runtime.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030E0000
#define Cix_PyStaticType_GetState _PyStaticType_GetState
#endif

#if PY_VERSION_HEX >= 0x030E0000
// Function prototypes for the huge list of redefinitions above that
// get imported before borrowed.h
_PyErr_StackItem* _PyErr_GetTopmostException(PyThreadState* tstate);
int _PyObject_HasLen(PyObject* o);

extern PyObject *Cix_monitoring_disable, *Cix_monitoring_missing;

int _Ci_Instrument(PyCodeObject* co, PyInterpreterState* interp);

Py_ssize_t _PyDict_LookupIndex(PyDictObject*, PyObject*);

Py_ssize_t _PyDictKeys_StringLookupSplit(PyDictKeysObject* dk, PyObject* key);
#endif

#if PY_VERSION_HEX >= 0x030C0000
#define Cix_PyGen_yf _PyGen_yf
#define Cix_PyCoro_GetAwaitableIter _PyCoro_GetAwaitableIter
#define Cix_Py_union_type_or _Py_union_type_or
#define Cix_PyCode_InitAddressRange _PyCode_InitAddressRange
#define Cix_PyLineTable_NextAddressRange _PyLineTable_NextAddressRange
#define Cix_PyDict_LoadGlobal _PyDict_LoadGlobal
#define Cix_PyThreadState_PushFrame _PyThreadState_PushFrame
#define Cix_PyThreadState_PopFrame _PyThreadState_PopFrame
#define Cix_PyFrame_ClearExceptCode _PyFrame_ClearExceptCode
#define Cix_PyTypeAlias_Type _PyTypeAlias_Type
#endif

#if PY_VERSION_HEX < 0x030C0000
// In 3.10 we create a new union object and grab the type and store it here.
extern PyTypeObject* Cix_PyUnion_Type;
#else
// In 3.12 _PyUnion_Type is exported, but it's hidden in an internal header
// file.
extern PyTypeObject _PyUnion_Type;
#define Cix_PyUnion_Type &_PyUnion_Type
#endif

PyObject* Cix_PyGen_yf(PyGenObject* gen);
PyObject* Cix_PyCoro_GetAwaitableIter(PyObject* o);
PyObject* Cix_PyAsyncGenValueWrapperNew(PyObject*);
#if PY_VERSION_HEX >= 0x030C0000
PyObject* Cix_compute_cr_origin(
    int origin_depth,
    _PyInterpreterFrame* current_frame);
#endif

PyObject* Cix_PyDict_LoadGlobal(
    PyDictObject* globals,
    PyDictObject* builtins,
    PyObject* key);

int Cix_PyObjectDict_SetItem(
    PyTypeObject* tp,
    PyObject* obj,
    PyObject** dictptr,
    PyObject* key,
    PyObject* value);

void Cix_PyDict_SendEvent(
    int watcher_bits,
    PyDict_WatchEvent event,
    PyDictObject* mp,
    PyObject* key,
    PyObject* value);

int Cix_set_attribute_error_context(PyObject* v, PyObject* name);

void Cix_dict_insert_split_value(
    PyInterpreterState* interp,
    PyDictObject* mp,
    PyObject* key,
    PyObject* value,
    Py_ssize_t ix);

// TODO: Get rid of this
#if PY_VERSION_HEX < 0x030F0000
#include "internal/pycore_tuple.h"
#define Cix_PyTuple_FromArray _PyTuple_FromArray
#else
#define Cix_PyTuple_FromArray PyTuple_FromArray
#endif

#if PY_VERSION_HEX >= 0x030C0000

// managed_static_type_state was known as static_builtin_state only in 3.12.
#if PY_VERSION_HEX < 0x030D0000
typedef static_builtin_state managed_static_type_state;
#endif

managed_static_type_state* Cix_PyStaticType_GetState(
    PyInterpreterState*,
    PyTypeObject*);
#endif

PyObject* Cix_Py_union_type_or(PyObject*, PyObject*);

#if PY_VERSION_HEX >= 0x030C0000
_PyInterpreterFrame* Cix_PyThreadState_PushFrame(
    PyThreadState* tstate,
    size_t size);

void Cix_PyThreadState_PopFrame(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame);

void Cix_PyFrame_ClearExceptCode(_PyInterpreterFrame* frame);

uint8_t Cix_DEINSTRUMENT(uint8_t op);

int Cix_PyCode_InitAddressRange(PyCodeObject* co, PyCodeAddressRange* bounds);
int Cix_PyLineTable_NextAddressRange(PyCodeAddressRange* range);
#endif

// There's some kind of issue with the borrow script in 3.10.cinder where it
// fails to copy these out correctly. However, it seems fine in 3.12.
// For 3.10.cinder we modify ceval.c.
#if PY_VERSION_HEX >= 0x030C0000
PyObject* Cix_match_class(
    PyThreadState* tstate,
    PyObject* subject,
    PyObject* type,
    Py_ssize_t nargs,
    PyObject* kwargs);
PyObject* Cix_match_keys(PyThreadState* tstate, PyObject* map, PyObject* keys);
void Cix_format_kwargs_error(
    PyThreadState* tstate,
    PyObject* func,
    PyObject* kwargs);
void Cix_format_exc_check_arg(
    PyThreadState* tstate,
    PyObject* exc,
    const char* format_str,
    PyObject* obj);
#endif

#if PY_VERSION_HEX >= 0x030C0000
PyObject* Cix_gc_freeze_impl(PyObject* module);
#endif

#if PY_VERSION_HEX >= 0x030C0000
PyObject* Ci_Builtin_Next_Core(PyObject* it, PyObject* def);
#endif

#if PY_VERSION_HEX >= 0x030C0000
void Cix_gen_dealloc_with_custom_free(PyObject* obj);
#endif

#if PY_VERSION_HEX >= 0x030E0000
uint32_t _CiDict_GetKeysVersionForCurrentState(
    PyInterpreterState* interp,
    PyDictObject* dict);

extern PyDictKeysObject* ci_dict_empty_keys;
#endif

#ifdef ENABLE_PEP523_HOOK
extern _PyFrameEvalFunction Ci_EvalFrameFunc;
#endif

int init_upstream_borrow(void);

#ifdef __cplusplus
} // extern "C"
#endif
