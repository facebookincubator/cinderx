// Copyright (c) Meta Platforms, Inc. and affiliates.

// C APIs to ModuleState.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Copy of CPython's vectorcall implementation for PyFunctionObject.
#if PY_VERSION_HEX >= 0x030F0000
#define Ci_PyFunction_Vectorcall _PyFunction_Vectorcall
#else
extern vectorcallfunc Ci_PyFunction_Vectorcall;
#endif

// WatcherState.

int Ci_Watchers_WatchDict(PyObject* dict);
int Ci_Watchers_UnwatchDict(PyObject* dict);

int Ci_Watchers_WatchType(PyTypeObject* type);
int Ci_Watchers_UnwatchType(PyTypeObject* type);

// GlobalCacheManager.

PyObject**
Ci_GetGlobalCache(PyObject* builtins, PyObject* globals, PyObject* key);

PyObject** Ci_GetDictCache(PyObject* dict, PyObject* key);

void Ci_free_jit_list_gen(PyGenObject* obj);

#ifdef __cplusplus
} // extern "C"
#endif
