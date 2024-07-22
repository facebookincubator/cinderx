// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TYPED_INT_UNSIGNED 0
#define TYPED_INT_SIGNED 1

#define TYPED_INT_8BIT 0
#define TYPED_INT_16BIT 1
#define TYPED_INT_32BIT 2
#define TYPED_INT_64BIT 3

#define TYPED_INT8 (TYPED_INT_8BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT16 (TYPED_INT_16BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT32 (TYPED_INT_32BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT64 (TYPED_INT_64BIT << 1 | TYPED_INT_SIGNED)

#define TYPED_UINT8 (TYPED_INT_8BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT16 (TYPED_INT_16BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT32 (TYPED_INT_32BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT64 (TYPED_INT_64BIT << 1 | TYPED_INT_UNSIGNED)

// Gets one of TYPED_INT_8BIT, TYPED_INT_16BIT, etc.. from TYPED_INT8,
// TYPED_UINT8, etc... also TYPED_SIZE(TYPED_BOOL) == TYPED_INT_8BIT
#define TYPED_SIZE(typed_int)  ((typed_int>>1) & 3)

#define TYPED_OBJECT 0x08
#define TYPED_DOUBLE 0x09
#define TYPED_SINGLE 0x0A
#define TYPED_CHAR 0x0B
// must be even: TYPED_BOOL & TYPED_INT_SIGNED should be false
#define TYPED_BOOL 0x0C
#define TYPED_VOID 0x0D
#define TYPED_STRING 0x0E
#define TYPED_ERROR 0x0F

#define TYPED_ARRAY 0x80

#define _Py_IS_TYPED_ARRAY(x) (x & TYPED_ARRAY)
#define _Py_IS_TYPED_ARRAY_SIGNED(x) (x & (TYPED_INT_SIGNED << 4))

Py_ssize_t _PyClassLoader_PrimitiveTypeToSize(int primitive_type);

int _PyClassLoader_PrimitiveTypeToStructMemberType(int type);

PyObject * _PyClassLoader_Box(uint64_t value, int primitive_type);
uint64_t _PyClassLoader_Unbox(PyObject *value, int primitive_type);

int _PyClassLoader_GetTypeCode(PyTypeObject *type);

static inline
int _PyObject_TypeCheckOptional(PyObject *val, PyTypeObject *type, int optional, int exact) {
    return Py_TYPE(val) == type ||
           (optional && val == Py_None) ||
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

#ifdef __cplusplus
}
#endif
