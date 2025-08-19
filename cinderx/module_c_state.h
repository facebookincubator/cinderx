// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// C APIs to ModuleState.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Copy of CPython's vectorcall implementation for PyFunctionObject.
extern vectorcallfunc Ci_PyFunction_Vectorcall;

// WatcherState.

int Ci_Watchers_WatchDict(PyObject* dict);
int Ci_Watchers_UnwatchDict(PyObject* dict);

int Ci_Watchers_WatchType(PyTypeObject* type);
int Ci_Watchers_UnwatchType(PyTypeObject* type);

// GlobalCacheManager.

PyObject**
Ci_GetGlobalCache(PyObject* builtins, PyObject* globals, PyObject* key);

PyObject** Ci_GetDictCache(PyObject* dict, PyObject* key);

#ifdef __cplusplus
} // extern "C"
#endif
