// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

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


int init_upstream_borrow(void);

#ifdef __cplusplus
} // extern "C"
#endif
