// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_frame.h"
#include "internal/pycore_typeobject.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PY_VERSION_HEX >= 0x030C0000
#define Cix_PyGen_yf _PyGen_yf
#define Cix_PyCoro_GetAwaitableIter _PyCoro_GetAwaitableIter
#define Cix_Py_union_type_or _Py_union_type_or
#define Cix_PyStaticType_GetState _PyStaticType_GetState
#define Cix_PyCode_InitAddressRange _PyCode_InitAddressRange
#define Cix_PyLineTable_NextAddressRange _PyLineTable_NextAddressRange
#define Cix_PyObjectDict_SetItem _PyObjectDict_SetItem
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

// TODO: Get rid of this
#include "internal/pycore_tuple.h"
#define Cix_PyTuple_FromArray _PyTuple_FromArray

#if PY_VERSION_HEX >= 0x030C0000
static_builtin_state* Cix_PyStaticType_GetState(
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

int Cix_do_raise(PyThreadState* tstate, PyObject* exc, PyObject* cause);

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

int init_upstream_borrow(void);

#ifdef __cplusplus
} // extern "C"
#endif
