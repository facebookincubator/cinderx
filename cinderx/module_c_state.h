// Copyright (c) Meta Platforms, Inc. and affiliates.

// C APIs to ModuleState.

#pragma once

#include "cinderx/python.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copy of CPython's vectorcall implementation for PyFunctionObject.
#if PY_VERSION_HEX >= 0x030F0000

#include "internal/pycore_function.h"

#define Ci_PyFunction_Vectorcall _PyFunction_Vectorcall
#else
extern vectorcallfunc Ci_PyFunction_Vectorcall;
#endif

// Get the StaticTypeError exception type.
PyObject* Ci_GetStaticTypeError(void);

// Generic type instantiation cache.
PyObject* Ci_GetGenericInstCache(void);
void Ci_SetGenericInstCache(PyDictObject* cache);
void Ci_ClearGenericInstCache(void);

// Class loader type resolution cache.
PyObject* Ci_GetClassLoaderCache(void);
void Ci_SetClassLoaderCache(PyDictObject* cache);
void Ci_ClearClassLoaderCache(void);

// Class loader module-to-keys mapping for cache invalidation.
PyObject* Ci_GetClassLoaderCacheModuleToKeys(void);
void Ci_SetClassLoaderCacheModuleToKeys(PyDictObject* cache);
void Ci_ClearClassLoaderCacheModuleToKeys(void);

// Value cache for adaptive interpreter.
PyObject* Ci_GetValueCache(void);
void Ci_SetValueCache(PyListObject* cache);
void Ci_ClearValueCache(void);

// Value-to-index mapping.
PyObject* Ci_GetValueIndices(void);
void Ci_SetValueIndices(PyDictObject* indices);
void Ci_ClearValueIndices(void);

// Type index offset for value cache.
int32_t Ci_GetTypeIndexOffset(void);
void Ci_AddTypeIndexOffset(int32_t offset);

// dlopen handle cache (lib_name -> handle).
PyObject* Ci_GetDlopenCache(void);
void Ci_SetDlopenCache(PyDictObject* cache);

// dlsym address cache ((lib_name, symbol_name) -> address).
PyObject* Ci_GetDlsymCache(void);
void Ci_SetDlsymCache(PyDictObject* cache);

// Cached reference to __static__.native_utils.invoke_native.
PyObject* Ci_GetInvokeNativeHelper(void);
void Ci_SetInvokeNativeHelper(PyFunctionObject* helper);

// Cached reference to __static__._return_none.
PyObject* Ci_GetReturnNone(void);
void Ci_SetReturnNone(PyFunctionObject* obj);

// Cached PyCFunction for weakref callback.
PyObject* Ci_GetWeakrefCallback(void);
void Ci_SetWeakrefCallback(PyCFunctionObject* cb);

// Static Python audit hook installation flag.
bool Ci_GetSpAuditHookInstalled(void);
void Ci_SetSpAuditHookInstalled(bool installed);

// Cached "list index out of range" error string.
PyObject* Ci_GetIndexErr(void);
void Ci_SetIndexErr(PyUnicodeObject* err);

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
