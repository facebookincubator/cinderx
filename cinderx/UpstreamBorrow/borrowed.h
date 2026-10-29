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

#ifdef Py_GIL_DISABLED
#define _PyCode_GetTLBC _CiCode_GetTLBC
#define _PyDict_KeysSize _CiDict_KeysSize
#define _PyList_GetItemRef _CiList_GetItemRef
#define _PyList_GetItemRefNoLock _CiList_GetItemRefNoLock
#define _Py_ExplicitMergeRefcount _Ci_ExplicitMergeRefcount
#define _PyMem_FreeDelayed _CiMem_FreeDelayed
#define _PyMem_ProcessDelayed _CiMem_ProcessDelayed
#define _PyCode_Quicken _CiCode_Quicken
#define _Py_qsbr_advance _Ci_qsbr_advance
#define _Py_qsbr_shared_next _Ci_qsbr_shared_next
#define _Py_qsbr_poll _Ci_qsbr_poll
#define _Py_AddRefTotal _Ci_AddRefTotal
#endif

// In 3.15, many of these functions are properly exported by CPython with
// PyAPI_FUNC, so the renames are only needed for 3.14.
#define _PyEval_Vector _CiEval_Vector
// PyObject* _PyExc_CreateExceptionGroup(const char* msg_str, PyObject* excs);

#define _PyFloat_FromDouble_ConsumeInputs _CiFloat_FromDouble_ConsumeInputs

#define _PyInstrumentation_MISSING (*Cix_monitoring_missing)
#define _PyInstrumentation_DISABLE (*Cix_monitoring_disable)

#define _PyInstruction_GetLength _CiInstruction_GetLength

#if PY_VERSION_HEX < 0x030F0000
#define _Py_GetBaseCodeUnit _Ci_GetBaseCodeUnit
#define _PyErr_SetObject _CiErr_SetObject
#define _PyFrame_ClearExceptCode _CiFrame_ClearExceptCode
#define _PyObject_HasLen _CiPyObject_HasLen
#define _PyFrame_ClearLocals _CiFrame_ClearLocals

#define _PyStaticType_GetState Cix_PyStaticType_GetState
#define _PyObject_VirtualAlloc _CiVirtualAlloc
#define _PyThreadState_PushFrame _CiThreadState_PushFrame
#define _PyErr_GetTopmostException _CiErr_GetTopmostException
#define _PyType_CacheGetItemForSpecialization \
  _CiType_CacheGetItemForSpecialization
#define _PyType_CacheInitForSpecialization _CiType_CacheInitForSpecialization
#define _PyType_LookupRefAndVersion _CiType_LookupRefAndVersion
#define _PyBuildSlice_ConsumeRefs _CiBuildSlice_ConsumeRefs
#define _PyFunction_GetVersionForCurrentState \
  _CiFunction_GetVersionForCurrentState
#define _PyExc_CreateExceptionGroup _CiExc_CreateExceptionGroup
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
#define _Py_call_instrumentation_line _Ci_call_instrumentation_line
#define _Py_call_instrumentation_exc2 _Ci_call_instrumentation_exc2
#define _PyNumber_InPlacePowerNoMod _CiNumber_InPlacePowerNoMod
#define _PyNumber_PowerNoMod _CiNumber_PowerNoMod
#define _PyFrame_MakeAndSetFrameObject _CiFrame_MakeAndSetFrameObject
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
#define _PyEval_MonitorRaise _CiEval_MonitorRaise
#define _PyEval_FrameClearAndPop _CiEval_FrameClearAndPop
#define _PyType_Validate _CiType_Validate
#define _Py_Specialize_Call _Ci_Specialize_Call
#define _PyTraceBack_FromFrame _CiTraceBack_FromFrame
#define _Py_CalculateSuggestions _Ci_CalculateSuggestions
#define _PyDict_CheckConsistency _CiDict_CheckConsistency
#define _PyDict_SetItem_LockHeld _CiDict_SetItem_LockHeld
#define _PyDict_DelItem_KnownHash_LockHeld _CiDict_DelItem_KnownHash_LockHeld
#define _PyInterpreterState_GetConfig _CiInterpreterState_GetConfig
#define _PyObjectDict_SetItem Cix_PyObjectDict_SetItem
#else
#define Cix_PyObjectDict_SetItem _PyObjectDict_SetItem
#define Cix_compute_cr_origin _PyCoro_ComputeOrigin
#define Cix_DEINSTRUMENT _PyCode_Deinstrument
#define Cix_GetOriginalOpcode _PyCode_GetOriginalOpcode
#endif

#define _Py_dict_lookup_keep_lazy _Ci_dict_lookup_keep_lazy

// The 3.14 file is built with 3.14.3, which includes gh-142534 that defines
// this new macro.  The lack of it on earlier point releases breaks the build.
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030E0300
#define FT_ATOMIC_LOAD_PTR_CONSUME FT_ATOMIC_LOAD_PTR_ACQUIRE
#endif

#ifdef __cplusplus
extern "C" {
#endif
PyObject* _PyNumber_InPlacePowerNoMod(PyObject* lhs, PyObject* rhs);
PyObject* _PyNumber_PowerNoMod(PyObject* lhs, PyObject* rhs);
#ifdef __cplusplus
}
#endif

#if PY_VERSION_HEX >= 0x030F0000
// Map CinderX wrapper name to real CPython function
#define Cix_PyStaticType_GetState _PyStaticType_GetState
#endif

#endif

#if PY_VERSION_HEX >= 0x030D0000 && PY_VERSION_HEX < 0x030F0000
// _PyTuple_MaybeUntrack stopped being exported in 3.13 but is still extern.
// We export it in 3.15 w/ the parallel GC patch
#define _PyTuple_MaybeUntrack _CiTuple_MaybeUntrack
#endif

// Must come after the renames above: it pulls in pycore headers whose
// declarations must see the renamed symbols, otherwise the unrenamed
// declarations get pinned by include guards and the renamed uses fail.
#include "cinderx/Common/runtime_offsets.h"

#if PY_VERSION_HEX >= 0x030E0000
// Upstream static inlines that read interpreter/thread-state fields at
// compiled-in offsets break when the loaded runtime's layout differs (see
// cinderx/Common/runtime_offsets.h). Each block below force-includes the
// defining header first, so the upstream inline keeps its real name and the
// helper macros stay available, then redirects callers to a biased clone.
// This must stay after the renames above for the same include-guard reason.
#include "internal/pycore_global_objects.h"
#undef _Py_INTERP_CACHED_OBJECT
#define _Py_INTERP_CACHED_OBJECT(interp, NAME) \
  CiInterpPost(interp)->cached_objects.NAME
#undef _Py_INTERP_STATIC_OBJECT
#define _Py_INTERP_STATIC_OBJECT(interp, NAME) \
  CiInterpPost(interp)->static_objects.NAME

#ifdef Py_GIL_DISABLED
// pycore_freelist.h is not C++-clean (void* to PyObject* conversion), so
// this shim is C-only. No C++ TU calls the freelist APIs.
#ifndef __cplusplus
#include "internal/pycore_freelist.h"
static inline struct _Py_freelists* _Ci_freelists_get(void) {
  PyThreadState* tstate = _PyThreadState_GET();
  _Py_AssertHoldsTstate();
  return &CiTstatePost(tstate)->freelists;
}
#define _Py_freelists_GET _Ci_freelists_get
#endif // !__cplusplus

#include "internal/pycore_code.h"
// Mirrors upstream _PyCode_GetTLBCFast with a biased tlbc_index read; keep
// in sync with CPython's pycore_code.h.
static inline _Py_CODEUNIT* _CiCode_GetTLBCFast(
    PyThreadState* tstate,
    PyCodeObject* co) {
  _PyCodeArray* code = _PyCode_GetTLBCArray(co);
  int32_t idx = CiTstatePost(tstate)->tlbc_index;
  if (idx < code->size && code->entries[idx] != NULL) {
    return (_Py_CODEUNIT*)code->entries[idx];
  }
  return NULL;
}
#define _PyCode_GetTLBCFast _CiCode_GetTLBCFast

#include "internal/pycore_interpframe.h"
// Mirrors upstream _PyFrame_InitializeTLBC with a biased tlbc_index read;
// keep in sync with CPython's pycore_interpframe.h.
static inline void _CiFrame_InitializeTLBC(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyCodeObject* code) {
  _Py_CODEUNIT* tlbc = _PyCode_GetTLBCFast(tstate, code);
  if (tlbc == NULL) {
    // No thread-local bytecode exists for this thread yet; use the main
    // thread's copy, deferring thread-local bytecode creation to the
    // execution of RESUME.
    frame->instr_ptr = _PyCode_CODE(code);
    frame->tlbc_index = 0;
  } else {
    frame->instr_ptr = tlbc;
    frame->tlbc_index = CiTstatePost(tstate)->tlbc_index;
  }
}
#define _PyFrame_InitializeTLBC _CiFrame_InitializeTLBC

#if PY_VERSION_HEX < 0x030F0000
// The frame-push and executable-code inlines transitively execute the raw
// reads above: CPython's headers define them before these renames take
// effect, so their bodies keep calling the upstream helpers. Redirect them
// to biased clones as well. 3.14-only: the signatures differ on other
// versions (3.12 takes fewer arguments) and no other version currently
// skews. Keep each body in sync with CPython 3.14's headers.
// Mirrors _PyFrame_Initialize from pycore_interpframe.h.
static inline void _CiFrame_Initialize(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    _PyStackRef func,
    PyObject* locals,
    PyCodeObject* code,
    int null_locals_from,
    _PyInterpreterFrame* previous) {
  frame->previous = previous;
  frame->f_funcobj = func;
  frame->f_executable = PyStackRef_FromPyObjectNew(code);
  PyFunctionObject* func_obj =
      (PyFunctionObject*)PyStackRef_AsPyObjectBorrow(func);
  frame->f_builtins = func_obj->func_builtins;
  frame->f_globals = func_obj->func_globals;
  frame->f_locals = locals;
  frame->stackpointer = frame->localsplus + code->co_nlocalsplus;
  frame->frame_obj = NULL;
#ifdef Py_GIL_DISABLED
  _CiFrame_InitializeTLBC(tstate, frame, code);
#else
  (void)tstate;
  frame->instr_ptr = _PyCode_CODE(code);
#endif
  frame->return_offset = 0;
  frame->owner = FRAME_OWNED_BY_THREAD;
  frame->visited = 0;
#ifdef Py_DEBUG
  frame->lltrace = 0;
#endif
  for (int i = null_locals_from; i < code->co_nlocalsplus; i++) {
    frame->localsplus[i] = PyStackRef_NULL;
  }
}
#define _PyFrame_Initialize _CiFrame_Initialize

// Mirrors _PyFrame_PushUnchecked from pycore_interpframe.h. The datastack
// reads below target base PyThreadState fields, which never skew.
static inline _PyInterpreterFrame* _CiFrame_PushUnchecked(
    PyThreadState* tstate,
    _PyStackRef func,
    int null_locals_from,
    _PyInterpreterFrame* previous) {
  CALL_STAT_INC(frames_pushed);
  PyFunctionObject* func_obj =
      (PyFunctionObject*)PyStackRef_AsPyObjectBorrow(func);
  PyCodeObject* code = (PyCodeObject*)func_obj->func_code;
  _PyInterpreterFrame* new_frame = (_PyInterpreterFrame*)tstate->datastack_top;
  tstate->datastack_top += code->co_framesize;
  assert(tstate->datastack_top < tstate->datastack_limit);
  _CiFrame_Initialize(
      tstate, new_frame, func, NULL, code, null_locals_from, previous);
  return new_frame;
}
#define _PyFrame_PushUnchecked _CiFrame_PushUnchecked

// Mirrors _PyFrame_PushTrampolineUnchecked from pycore_interpframe.h.
static inline _PyInterpreterFrame* _CiFrame_PushTrampolineUnchecked(
    PyThreadState* tstate,
    PyCodeObject* code,
    int stackdepth,
    _PyInterpreterFrame* previous) {
  CALL_STAT_INC(frames_pushed);
  _PyInterpreterFrame* frame = (_PyInterpreterFrame*)tstate->datastack_top;
  tstate->datastack_top += code->co_framesize;
  assert(tstate->datastack_top < tstate->datastack_limit);
  frame->previous = previous;
  frame->f_funcobj = PyStackRef_None;
  frame->f_executable = PyStackRef_FromPyObjectNew(code);
#ifdef Py_DEBUG
  frame->f_builtins = NULL;
  frame->f_globals = NULL;
#endif
  frame->f_locals = NULL;
  assert(stackdepth <= code->co_stacksize);
  frame->stackpointer = frame->localsplus + code->co_nlocalsplus + stackdepth;
  frame->frame_obj = NULL;
#ifdef Py_GIL_DISABLED
  _CiFrame_InitializeTLBC(tstate, frame, code);
#else
  frame->instr_ptr = _PyCode_CODE(code);
#endif
  frame->owner = FRAME_OWNED_BY_THREAD;
  frame->visited = 0;
#ifdef Py_DEBUG
  frame->lltrace = 0;
#endif
  frame->return_offset = 0;
  return frame;
}
#define _PyFrame_PushTrampolineUnchecked _CiFrame_PushTrampolineUnchecked

// Forward declaration: _PyCode_GetTLBC is borrowed-and-renamed above, so
// both the upstream body below and the mirror expand it to
// _CiCode_GetTLBC, whose prototype otherwise only appears later in this
// file.
#ifdef __cplusplus
extern "C" _Py_CODEUNIT* _PyCode_GetTLBC(PyCodeObject* co);
#else
_Py_CODEUNIT* _PyCode_GetTLBC(PyCodeObject* co);
#endif
#include "internal/pycore_ceval.h"
// Mirrors _PyEval_GetExecutableCode from pycore_ceval.h.
static inline _Py_CODEUNIT* _CiEval_GetExecutableCode(
    PyThreadState* tstate,
    PyCodeObject* co) {
  _Py_CODEUNIT* bc = _CiCode_GetTLBCFast(tstate, co);
  if (bc != NULL) {
    return bc;
  }
  return _PyCode_GetTLBC(co);
}
#define _PyEval_GetExecutableCode _CiEval_GetExecutableCode
#endif // PY_VERSION_HEX < 0x030F0000
#endif // Py_GIL_DISABLED
#endif // PY_VERSION_HEX >= 0x030E0000

#include "internal/pycore_dict.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_typeobject.h"

#include "cinderx/python_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PY_VERSION_HEX < 0x030E0000
#define Cix_PyStaticType_GetState _PyStaticType_GetState
#endif

#if PY_VERSION_HEX >= 0x030E0000
// Function prototypes for the huge list of redefinitions above that
// get imported before borrowed.h

#ifdef Py_GIL_DISABLED
_Py_CODEUNIT* _PyCode_GetTLBC(PyCodeObject* co);
void _PyMem_FreeDelayed(void* ptr, size_t size);
void _PyMem_ProcessDelayed(PyThreadState* tstate);
#endif

_PyErr_StackItem* _PyErr_GetTopmostException(PyThreadState* tstate);
int _PyObject_HasLen(PyObject* o);

extern PyObject *Cix_monitoring_disable, *Cix_monitoring_missing;

int _Ci_Instrument(PyCodeObject* co, PyInterpreterState* interp);

Py_ssize_t _PyDict_LookupIndex(PyDictObject*, PyObject*);

Py_ssize_t _PyDictKeys_StringLookupSplit(PyDictKeysObject* dk, PyObject* key);

#include "internal/pycore_stackref.h"
PyAPI_FUNC(_PyStackRef) _PyFloat_FromDouble_ConsumeInputs(
    _PyStackRef left,
    _PyStackRef right,
    double value);

#endif

#define Cix_PyCode_InitAddressRange _PyCode_InitAddressRange
#define Cix_PyLineTable_NextAddressRange _PyLineTable_NextAddressRange
#define Cix_PyThreadState_PushFrame _PyThreadState_PushFrame
#define Cix_PyFrame_ClearExceptCode _PyFrame_ClearExceptCode
#define Cix_PyTypeAlias_Type _PyTypeAlias_Type

#ifndef WIN32
// _PyUnion_Type is exported, but it's hidden in an internal header file.
extern PyTypeObject _PyUnion_Type;
#define Cix_PyUnion_Type &_PyUnion_Type
#else
extern PyTypeObject* Cix_PyUnion_Type;
#endif

PyObject* Cix_PyAsyncGenValueWrapperNew(PyObject*);
PyObject* Cix_compute_cr_origin(
    int origin_depth,
    _PyInterpreterFrame* current_frame);

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

#if PY_VERSION_HEX < 0x030F0000
void _PyDict_InsertSplitValue(
    PyDictObject* mp,
    PyObject* key,
    PyObject* value,
    Py_ssize_t ix);
#endif

// TODO: Get rid of this
#if PY_VERSION_HEX < 0x030F0000
#include "internal/pycore_tuple.h"
#define Cix_PyTuple_FromArray _PyTuple_FromArray
#else
#define Cix_PyTuple_FromArray PyTuple_FromArray
#endif

// managed_static_type_state was known as static_builtin_state only in 3.12.
#if PY_VERSION_HEX < 0x030D0000
typedef static_builtin_state managed_static_type_state;
#endif

managed_static_type_state* Cix_PyStaticType_GetState(
    PyInterpreterState*,
    PyTypeObject*);

_PyInterpreterFrame* Cix_PyThreadState_PushFrame(
    PyThreadState* tstate,
    size_t size);

void Cix_PyFrame_ClearExceptCode(_PyInterpreterFrame* frame);

#if PY_VERSION_HEX < 0x030F0000
uint8_t Cix_DEINSTRUMENT(uint8_t op);
#endif

int Cix_PyCode_InitAddressRange(PyCodeObject* co, PyCodeAddressRange* bounds);
int Cix_PyLineTable_NextAddressRange(PyCodeAddressRange* range);

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

PyObject* Ci_Builtin_Next_Core(PyObject* it, PyObject* def);

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
PyObject* Cix_cr_getrunning(PyObject* self, void* ignored);
PyObject* Cix_cr_getsuspended(PyObject* self, void* ignored);
#endif

#if PY_VERSION_HEX >= 0x030E0000
uint32_t _CiDict_GetKeysVersionForCurrentState(
    PyInterpreterState* interp,
    PyDictObject* dict);

extern PyDictKeysObject* ci_dict_empty_keys;
#endif

// Access the CinderX evaluator used by borrowed specialization code to
// distinguish CinderX from other PEP 523 evaluators.
_PyFrameEvalFunction Ci_GetEvalFrameFunc(void);

int init_upstream_borrow(void);

// Function pointer for CPython's _Py_slot_tp_getattr_hook, which is installed
// as tp_getattro when a class defines __getattr__. Discovered at init time
// by creating a temporary class with __getattr__.
extern getattrofunc Ci_tp_getattr_hook;

#ifdef __cplusplus
} // extern "C"
#endif
