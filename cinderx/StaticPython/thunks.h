// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject _PyType_MethodThunk;
extern PyTypeObject _PyType_StaticThunk;
extern PyTypeObject _PyType_PropertyThunk;
extern PyTypeObject _PyType_TypedDescriptorThunk;
extern PyTypeObject _PyClassLoader_VTableInitThunk_Type;

#define THUNK_SIG(arg_count)      \
  {                               \
      .ta_argcount = arg_count,   \
      .ta_has_primitives = 0,     \
      .ta_allocated = 0,          \
      .ta_rettype = TYPED_OBJECT, \
  }

typedef struct {
  // Arg count shifted left by two, low bit indicates if we have typed
  // information, 2nd bit indicates that the signature was allocated and
  // needs to be freed.
  Py_ssize_t ta_argcount : 62;
  unsigned int ta_has_primitives : 1;
  unsigned int ta_allocated : 1;
  uint8_t ta_rettype;
  uint8_t ta_argtype[0];
} _PyClassLoader_ThunkSignature;

static inline void _PyClassLoader_FreeThunkSignature(
    _PyClassLoader_ThunkSignature* sig) {
  if (sig != NULL && sig->ta_allocated) {
    PyMem_Free(sig);
  }
}

// A thunk used for the initialization of a vtable-entry on
// the first time it's actually invoked. We'll then resolve what it should
// point to and replace the thunk w/ the appropriate function.
typedef struct {
  PyObject_HEAD
  PyObject* vti_name;
  PyTypeObject* vti_type;
  vectorcallfunc vti_call;
} _PyClassLoader_VTableInitThunk;

PyObject* _PyClassLoader_VTableInitThunk_New(
    PyObject* name,
    PyTypeObject* type,
    vectorcallfunc call);

typedef struct {
  PyObject_HEAD
  _PyClassLoader_ThunkSignature* mt_sig;
  vectorcallfunc mt_call;
} _PyClassLoader_MethodThunk;

/**
    In order to ensure sanity of types at runtime, we need to check the return
   values of functions and ensure they remain compatible with the declared
   return type (even if the callable is patched).

    This structure helps us do that.
*/
typedef struct {
  _PyClassLoader_MethodThunk rt_base;
  PyTypeObject* rt_expected;
  PyObject* rt_name;
  int rt_optional;
  int rt_exact;
} _PyClassLoader_RetTypeInfo;

typedef struct {
  _PyClassLoader_RetTypeInfo tcs_rt;
  PyObject* tcs_value;
} _PyClassLoader_TypeCheckThunk;

PyObject* _PyClassLoader_TypeCheckThunk_New(
    PyObject* value,
    PyObject* name,
    PyTypeObject* ret_type,
    int optional,
    int exact,
    _PyClassLoader_ThunkSignature* sig);

typedef struct {
  _PyClassLoader_MethodThunk smt_base;
  PyObject* smt_func;
} _PyClassLoader_StaticMethodThunk;

PyObject* _PyClassLoader_StaticMethodThunk_New(
    PyObject* func,
    _PyClassLoader_ThunkSignature* sig,
    vectorcallfunc call);

typedef struct {
  _PyClassLoader_MethodThunk cmt_base;
  PyTypeObject* cmt_decl_type;
  PyObject* cmt_classmethod;
} _PyClassLoader_ClassMethodThunk;

PyObject* _PyClassLoader_ClassMethodThunk_New(
    PyObject* original,
    _PyClassLoader_ThunkSignature* code,
    PyTypeObject* decl_type,
    vectorcallfunc call);

// Thunk state used for a function which hasn't been initialized yet. These
// functions will have an entry for auto-JITing either on the first call or
// after they are warmed up. We need to update the v-table when the function is
// JITed and replace it with the direct entry point.
typedef struct {
  _PyClassLoader_MethodThunk lf_base;
  PyFunctionObject* lf_func;
  PyObject* lf_vtable;
  Py_ssize_t lf_slot;
} _PyClassLoader_LazyFuncJitThunk;

PyObject* _PyClassLoader_LazyFuncJitThunk_New(
    PyObject* vtable,
    Py_ssize_t slot,
    PyFunctionObject* original,
    _PyClassLoader_ThunkSignature* code,
    vectorcallfunc call);

typedef struct {
  _PyClassLoader_TypeCheckThunk thunk_tcs;
  /* the class that the thunk exists for (used for error reporting) */
  PyTypeObject* thunk_cls;
  /* Function type: coroutine, static method, class method */
  int thunk_flags;
  /* a pointer which can be used for an indirection in
   * *PyClassLoader_GetIndirectPtr. This will be the current value of the
   * function when it's not patched and will be the thunk when it is. */
  PyObject* thunk_funcref; /* borrowed */
  /* the vectorcall entry point for the thunk */
  vectorcallfunc thunk_vectorcall;
} _Py_StaticThunk;

typedef struct {
  PyObject_HEAD
  PyObject* propthunk_target;
  /* the vectorcall entry point for the thunk */
  vectorcallfunc propthunk_vectorcall;
} _Py_PropertyThunk;

typedef enum { THUNK_SETTER, THUNK_GETTER, THUNK_DELETER } PropertyThunkKind;

PyObject* _PyClassLoader_PropertyThunk_GetProperty(PyObject* thunk);
extern PyObject* _PyClassLoader_PropertyThunkGet_New(PyObject* property);
extern PyObject* _PyClassLoader_PropertyThunkSet_New(PyObject* property);
extern PyObject* _PyClassLoader_PropertyThunkDel_New(PyObject* property);
PropertyThunkKind _PyClassLoader_PropertyThunk_Kind(PyObject* property);

void _PyClassLoader_UpdateThunk(
    _Py_StaticThunk* thunk,
    PyObject* previous,
    PyObject* new_value);

#ifdef __cplusplus
}
#endif
