// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Copy of CPython's vectorcall implementation for PyFunctionObject.
extern vectorcallfunc Ci_PyFunction_Vectorcall;

// C API for WatcherState.

int Ci_Watchers_WatchDict(PyObject* dict);
int Ci_Watchers_UnwatchDict(PyObject* dict);

int Ci_Watchers_WatchType(PyTypeObject* type);
int Ci_Watchers_UnwatchType(PyTypeObject* type);

#ifdef __cplusplus
} // extern "C"
#endif
