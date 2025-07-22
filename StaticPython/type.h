// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

Py_ssize_t _PyClassLoader_PrimitiveTypeToSize(int primitive_type);

int _PyClassLoader_PrimitiveTypeToStructMemberType(int type);

PyObject* _PyClassLoader_Box(uint64_t value, int primitive_type);
uint64_t _PyClassLoader_Unbox(PyObject* value, int primitive_type);

static inline int _PyObject_TypeCheckOptional(
    PyObject* val,
    PyTypeObject* type,
    int optional,
    int exact) {
  return Py_TYPE(val) == type || (optional && val == Py_None) ||
      (!exact && PyObject_TypeCheck(val, type));
}

PyObject* _PyClassLoader_GetModuleAttr(PyObject* module, PyObject* name);

// Resolves a type descriptor in the form to the underlying class or module.
// The descriptor could take various forms:
//      ('module', 'Class')           - simple class reference
//      ('module', )                  - simple module reference
//      ('module', 'Class', ('T', ))  - generic type
//      ('__static__', 'int64', '#')  - primitive
//      ('module', 'Class', '!')      - exact type
PyObject* _PyClassLoader_ResolveContainer(PyObject* container_path);

int _PyClassLoader_VerifyType(PyObject* type, PyObject* path);

PyTypeObject*
_PyClassLoader_ResolveType(PyObject* descr, int* optional, int* exact);

// Checks to see if a modification to a dictionary is updating sys.modules
// and if so invalidates any associated caches.
int _PyClassLoader_CheckModuleChange(PyDictObject* dict, PyObject* key);

// Clears the class loader cache which is used to cache resolutions of members.
void _PyClassLoader_ClearCache(void);

// Gets the class loader cache which is used to cache resolutions of members.
PyObject* _PyClassLoader_GetCache();

int _PyClassLoader_ResolvePrimitiveType(PyObject* descr);

int is_static_type(PyTypeObject* type);

#ifdef __cplusplus
}
#endif
