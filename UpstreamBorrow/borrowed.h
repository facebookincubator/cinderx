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

int
Cix_set_attribute_error_context(PyObject *v, PyObject *name);

#ifdef __cplusplus
} // extern "C"
#endif
