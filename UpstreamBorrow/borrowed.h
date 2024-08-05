// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "pycore_typeobject.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

PyObject *
Cix_PyGen_yf(PyGenObject *gen);
PyObject*
Cix_PyCoro_GetAwaitableIter(PyObject *o);
PyObject*
Cix_PyAsyncGenValueWrapperNew(PyObject *);

PyObject *
Cix_PyDict_LoadGlobal(PyDictObject *globals, PyDictObject *builtins, PyObject *key);

int
Cix_PyObjectDict_SetItem(PyTypeObject *tp, PyObject **dictptr,
                      PyObject *key, PyObject *value);

int
Cix_set_attribute_error_context(PyObject *v, PyObject *name);


#if PY_VERSION_HEX >= 0x030C0000
PyObject* Cix_PyTuple_FromArray(PyObject *const *, Py_ssize_t);
#else
#include "internal/pycore_tuple.h"
#define Cix_PyTuple_FromArray _PyTuple_FromArray
#endif

#if PY_VERSION_HEX >= 0x030C0000
static_builtin_state* Cix_PyStaticType_GetState(PyInterpreterState *, PyTypeObject *);
#endif

int init_upstream_borrow(void);

#ifdef __cplusplus
} // extern "C"
#endif
