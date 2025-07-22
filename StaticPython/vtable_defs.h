/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include "cinderx/python.h"

#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/vtable.h"

#ifdef __cplusplus
extern "C" {
#endif

static const _PyClassLoader_StaticCallReturn StaticError = {0, 0};

// Converts native arguments that were pushed onto the stack into an
// array of PyObject** args that can be used to call into the interpreter.
int _PyClassLoader_HydrateArgsFromSig(
    _PyClassLoader_ThunkSignature* sig,
    Py_ssize_t arg_count,
    void** args,
    PyObject** call_args,
    PyObject** free_args);

// Frees the arguments which were hydrated with _PyClassLoader_HydrateArgs
void _PyClassLoader_FreeHydratedArgs(
    PyObject** free_args,
    Py_ssize_t arg_count);

// Gets the underlying signature for a function, going through things like
// class methods and static methods.
_PyClassLoader_ThunkSignature* _PyClassLoader_GetThunkSignature(
    PyObject* original);

_PyClassLoader_ThunkSignature* _PyClassLoader_CopyThunkSig(
    _PyClassLoader_ThunkSignature* sig);

PyObject* _PyVTable_coroutine_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);

PyObject* _PyVTable_func_lazyinit_vectorcall(
    _PyClassLoader_LazyFuncJitThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);
PyObject* _PyVTable_coroutine_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_nonfunc_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_func_typecheck_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_descr_typecheck_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_coroutine_vectorcall_no_self(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_staticmethod_vectorcall(
    _PyClassLoader_StaticMethodThunk* method,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_classmethod_vectorcall(
    _PyClassLoader_ClassMethodThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_func_missing_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);

_PyClassLoader_StaticCallReturn _PyVTable_func_lazyinit_native(
    _PyClassLoader_LazyFuncJitThunk* state,
    void** args);

PyObject* _PyVTable_func_lazyinit_vectorcall(
    _PyClassLoader_LazyFuncJitThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);

PyObject* _PyVTable_thunk_ret_primitive_not_jitted_vectorcall(
    _PyClassLoader_LazyFuncJitThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);

PyObject* _PyVTable_native_entry(PyObject* state, void** args);

StaticMethodInfo _PyVTable_load_descr_typecheck(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self);
StaticMethodInfo _PyVTable_classmethod_load_overridable(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self);
StaticMethodInfo _PyVTable_load_overridable(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self);
StaticMethodInfo _PyVTable_load_jitted_func(PyObject* state, PyObject* self);
StaticMethodInfo _PyVTable_load_descr(PyObject* state, PyObject* self);
StaticMethodInfo _PyVTable_load_generic(PyObject* state, PyObject* self);

#ifdef __cplusplus
}
#endif
