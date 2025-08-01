// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable.h"

#ifdef __cplusplus
extern "C" {
#endif

_PyType_VTable* _PyClassLoader_EnsureVtable(
    PyTypeObject* self,
    int init_subclasses);

int _PyClassLoader_InitSubclassVtables(PyTypeObject* target_type);

extern PyObject* _PyClassLoader_ResolveMember(
    PyObject* path,
    Py_ssize_t items,
    PyObject** container,
    PyObject** containerkey);

int _PyClassLoader_GetStaticallyInheritedMember(
    PyTypeObject* type,
    PyObject* name,
    PyObject** result);

int _PyClassLoader_InitTypeForPatching(PyTypeObject* type);

int _PyClassLoader_UpdateSlot(
    PyTypeObject* type,
    PyObject* name,
    PyObject* new_value);

static inline int _PyClassLoader_IsStaticCallable(PyObject* obj) {
  return _PyClassLoader_IsStaticFunction(obj) ||
      _PyClassLoader_IsStaticBuiltin(obj);
}

int _PyClassLoader_CheckSubclassChange(
    PyDictObject* dict,
    PyDict_WatchEvent event,
    PyObject* key,
    PyObject* value);

int _PyClassLoader_SetTypeStatic(PyTypeObject* type);

#ifdef __cplusplus
}
#endif
