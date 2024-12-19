// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/typed-args-info.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int _PyClassLoader_IsStaticFunction(PyObject* obj) {
  if (obj == NULL || !PyFunction_Check(obj)) {
    return 0;
  }
  return ((PyCodeObject*)(((PyFunctionObject*)obj))->func_code)->co_flags &
      CI_CO_STATICALLY_COMPILED;
}

PyObject* _PyClassLoader_ResolveReturnType(
    PyObject* func,
    int* optional,
    int* exact,
    int* func_flags);

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfo(
    PyCodeObject* code,
    int only_primitives);

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfoFromThunk(
    PyObject* thunk,
    PyObject* container,
    int only_primitives);

PyObject* _PyClassLoader_GetReturnTypeDescr(PyFunctionObject* func);
PyObject* _PyClassLoader_GetCodeReturnTypeDescr(PyCodeObject* code);
PyObject* _PyClassLoader_GetCodeArgumentTypeDescrs(PyCodeObject* code);
PyObject* _PyClassLoader_CheckReturnType(
    PyTypeObject* cls,
    PyObject* ret,
    _PyClassLoader_RetTypeInfo* rt_info);
PyObject* _PyClassLoader_CheckReturnCallback(
    _PyClassLoader_Awaitable* awaitable,
    PyObject* result);

int _PyClassLoader_IsPropertyName(PyTupleObject* name);
PyObject* _PyClassLoader_GetFunctionName(PyObject* name);

#define Ci_FUNC_FLAGS_COROUTINE 0x01
#define Ci_FUNC_FLAGS_CLASSMETHOD 0x02
#define Ci_FUNC_FLAGS_STATICMETHOD 0x04

PyObject* _PyClassLoader_MaybeUnwrapCallable(PyObject* func);

PyObject* _PyClassLoader_CallCoroutine(
    _PyClassLoader_TypeCheckState* state,
    PyObject* const* args,
    size_t nargsf);

PyObject* _PyClassLoader_CallCoroutineOverridden(
    _PyClassLoader_TypeCheckState* state,
    PyObject* func,
    PyObject* const* args,
    size_t nargsf);

#ifdef __cplusplus
}
#endif
