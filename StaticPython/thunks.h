// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject _PyType_CachedPropertyThunk;
extern PyTypeObject _PyType_AsyncCachedPropertyThunk;
extern PyTypeObject _PyType_StaticThunk;
extern PyTypeObject _PyType_PropertyThunk;
extern PyTypeObject _PyType_TypedDescriptorThunk;

typedef struct {
  PyObject_HEAD
  PyObject* mt_original;
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
} _PyClassLoader_TypeCheckState;

typedef struct {
  PyObject_HEAD
  PyObject* propthunk_target;
  /* the vectorcall entry point for the thunk */
  vectorcallfunc propthunk_vectorcall;
} _Py_AsyncCachedPropertyThunk;

extern _Py_AsyncCachedPropertyThunk* _Py_AsyncCachedPropertyThunk_New(
    PyObject* property);
extern PyObject* _Py_AsyncCachedPropertyThunk_GetFunc(PyObject* thunk);

typedef struct {
  _PyClassLoader_TypeCheckState thunk_tcs;
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
} _Py_CachedPropertyThunk;

extern _Py_CachedPropertyThunk* _Py_CachedPropertyThunk_New(PyObject* property);
extern PyObject* _Py_CachedPropertyThunk_GetFunc(PyObject* thunk);

typedef struct {
  PyObject_HEAD
  PyObject* propthunk_target;
  /* the vectorcall entry point for the thunk */
  vectorcallfunc propthunk_vectorcall;
} _Py_PropertyThunk;

typedef struct {
  PyObject_HEAD
  PyObject* typed_descriptor_thunk_target;
  /* the vectorcall entry point for the thunk */
  vectorcallfunc typed_descriptor_thunk_vectorcall;
  int is_setter;
} _Py_TypedDescriptorThunk;

extern PyObject* _PyClassLoader_PropertyThunkGet_New(PyObject* property);
extern PyObject* _PyClassLoader_PropertyThunkSet_New(PyObject* property);

extern PyObject* _PyClassLoader_TypedDescriptorThunkGet_New(PyObject* property);
extern PyObject* _PyClassLoader_TypedDescriptorThunkSet_New(PyObject* property);

void
_PyClassLoader_UpdateThunk(_Py_StaticThunk* thunk, PyObject* previous, PyObject* new_value);

#ifdef __cplusplus
}
#endif
