/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#ifndef Ci_CLASSLOADER_H
#define Ci_CLASSLOADER_H

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "cinder/hooks.h"
#endif

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/generic_type.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed-args-info.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable.h"

#ifdef __cplusplus
extern "C" {
#endif

extern PyObject* CiExc_StaticTypeError;

#ifndef Py_LIMITED_API

#endif

Py_ssize_t _PyClassLoader_ResolveMethod(PyObject* path);
Py_ssize_t _PyClassLoader_ResolveFieldOffset(PyObject* path, int* field_type);
int _PyClassLoader_GetTypeCode(PyTypeObject* type);

int _PyClassLoader_AddSubclass(PyTypeObject* base, PyTypeObject* type);

_PyType_VTable* _PyClassLoader_EnsureVtable(
    PyTypeObject* self,
    int init_subclasses);
int _PyClassLoader_ClearVtables(void);
void _PyClassLoader_ClearGenericTypes(void);

int _PyClassLoader_IsPatchedThunk(PyObject* obj);

/* Gets an indirect pointer for a function.  This should be used if
 * the given container is mutable, and the indirect pointer will
 * track changes to the object.  If changes are unable to be tracked
 * the pointer will start pointing at NULL and the function will need
 * to be re-resolved.
 */
PyObject** _PyClassLoader_ResolveIndirectPtr(PyObject* path);

/* Resolves a function and returns the underlying object and the
 * container.  Static functions return the underlying callable */
PyObject* _PyClassLoader_ResolveFunction(PyObject* path, PyObject** container);

PyMethodDescrObject* _PyClassLoader_ResolveMethodDef(PyObject* path);

/* Checks whether any method in the members dict overrides a final method in the
   base type. This API explicitly takes in a base_type and members_dict instead
   of a type object as it is used within `type_new`.
 */
int _PyClassLoader_IsFinalMethodOverridden(
    PyTypeObject* base_type,
    PyObject* members_dict);

#define PRIM_OP_ADD_INT 0
#define PRIM_OP_SUB_INT 1
#define PRIM_OP_MUL_INT 2
#define PRIM_OP_DIV_INT 3
#define PRIM_OP_DIV_UN_INT 4
#define PRIM_OP_MOD_INT 5
#define PRIM_OP_MOD_UN_INT 6
#define PRIM_OP_POW_INT 7
#define PRIM_OP_LSHIFT_INT 8
#define PRIM_OP_RSHIFT_INT 9
#define PRIM_OP_RSHIFT_UN_INT 10
#define PRIM_OP_XOR_INT 11
#define PRIM_OP_OR_INT 12
#define PRIM_OP_AND_INT 13

#define PRIM_OP_ADD_DBL 14
#define PRIM_OP_SUB_DBL 15
#define PRIM_OP_MUL_DBL 16
#define PRIM_OP_DIV_DBL 17
#define PRIM_OP_MOD_DBL 18
#define PRIM_OP_POW_DBL 19
#define PRIM_OP_POW_UN_INT 20

#define PRIM_OP_EQ_INT 0
#define PRIM_OP_NE_INT 1
#define PRIM_OP_LT_INT 2
#define PRIM_OP_LE_INT 3
#define PRIM_OP_GT_INT 4
#define PRIM_OP_GE_INT 5
#define PRIM_OP_LT_UN_INT 6
#define PRIM_OP_LE_UN_INT 7
#define PRIM_OP_GT_UN_INT 8
#define PRIM_OP_GE_UN_INT 9
#define PRIM_OP_EQ_DBL 10
#define PRIM_OP_NE_DBL 11
#define PRIM_OP_LT_DBL 12
#define PRIM_OP_LE_DBL 13
#define PRIM_OP_GT_DBL 14
#define PRIM_OP_GE_DBL 15

#define PRIM_OP_NEG_INT 0
#define PRIM_OP_INV_INT 1
#define PRIM_OP_NEG_DBL 2
#define PRIM_OP_NOT_INT 3

#define FAST_LEN_INEXACT (1 << 4)
#define FAST_LEN_LIST 0
#define FAST_LEN_DICT 1
#define FAST_LEN_SET 2
#define FAST_LEN_TUPLE 3
#define FAST_LEN_ARRAY 4
#define FAST_LEN_STR 5

// At the time of defining these, we needed to remain backwards compatible,
// so SEQ_LIST had to be zero. Therefore, we let the array types occupy the
// higher nibble and the lower nibble is for defining other sequence types
// (top bit of lower nibble is for unchecked flag).
#define SEQ_LIST 0
#define SEQ_TUPLE 1
#define SEQ_LIST_INEXACT 2
#define SEQ_SUBSCR_UNCHECKED (1 << 3)
#define SEQ_REPEAT_INEXACT_SEQ (1 << 4)
#define SEQ_REPEAT_INEXACT_NUM (1 << 5)
#define SEQ_REPEAT_REVERSED (1 << 6)
#define SEQ_REPEAT_PRIMITIVE_NUM (1 << 7)

// For arrays, the constant contains the element type in the higher
// nibble
#define SEQ_ARRAY_INT64 ((TYPED_INT64 << 4) | TYPED_ARRAY)
#define SEQ_REPEAT_FLAGS                                                   \
  (SEQ_REPEAT_INEXACT_SEQ | SEQ_REPEAT_INEXACT_NUM | SEQ_REPEAT_REVERSED | \
   SEQ_REPEAT_PRIMITIVE_NUM)
#define SEQ_CHECKED_LIST (1 << 8)

#define Ci_Py_SIG_INT8 (TYPED_INT8 << 2)
#define Ci_Py_SIG_INT16 (TYPED_INT16 << 2)
#define Ci_Py_SIG_INT32 (TYPED_INT32 << 2)
#define Ci_Py_SIG_INT64 (TYPED_INT64 << 2)
#define Ci_Py_SIG_UINT8 (TYPED_UINT8 << 2)
#define Ci_Py_SIG_UINT16 (TYPED_UINT16 << 2)
#define Ci_Py_SIG_UINT32 (TYPED_UINT32 << 2)
#define Ci_Py_SIG_UINT64 (TYPED_UINT64 << 2)
#define Ci_Py_SIG_OBJECT (TYPED_OBJECT << 2)
#define Ci_Py_SIG_VOID (TYPED_VOID << 2)
#define Ci_Py_SIG_STRING (TYPED_STRING << 2)
#define Ci_Py_SIG_ERROR (TYPED_ERROR << 2)
#define Ci_Py_SIG_SSIZE_T \
  (sizeof(void*) == 8 ? Ci_Py_SIG_INT64 : Ci_Py_SIG_INT32)
#define Ci_Py_SIG_SIZE_T \
  (sizeof(void*) == 8 ? Ci_Py_SIG_UINT64 : Ci_Py_SIG_UINT32)
#define Ci_Py_SIG_TYPE_MASK(x) ((x) >> 2)

#ifndef Py_LIMITED_API

PyObject* _PyClassloader_SizeOf_DlSym_Cache(void);
PyObject* _PyClassloader_SizeOf_DlOpen_Cache(void);
void _PyClassloader_Clear_DlSym_Cache(void);
void _PyClassloader_Clear_DlOpen_Cache(void);

void* _PyClassloader_LookupSymbol(PyObject* lib_name, PyObject* symbol_name);

int _PyClassLoader_AddSubclass(PyTypeObject* base, PyTypeObject* type);

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfoFromThunk(
    PyObject* thunk,
    PyObject* container,
    int only_primitives);
int _PyClassLoader_HasPrimitiveArgs(PyCodeObject* code);

int _PyClassLoader_NotifyDictChange(
    PyDictObject* dict,
    PyDict_WatchEvent event,
    PyObject* key,
    PyObject* value);

PyObject* _PyClassloader_InvokeNativeFunction(
    PyObject* lib_name,
    PyObject* symbol_name,
    PyObject* signature,
    PyObject** args,
    Py_ssize_t nargs);

PyObject* _PyClassLoader_InvokeMethod(
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargsf);

// Caches a value for the adaptive interpreter
int32_t _PyClassLoader_CacheValue(PyObject* type);

// Gets a cached value for the adaptive interpreter
PyObject* _PyClassLoader_GetCachedValue(int32_t type);

void _PyClassLoader_ClearValueCache();

#endif

#ifdef __cplusplus
}
#endif
#endif /* !Ci_CLASSLOADER_H */
