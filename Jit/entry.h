// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#ifdef __cplusplus
namespace jit {

void initFunctionObjectForJIT(PyFunctionObject* func);

} // namespace jit

extern "C" {
#endif // __cplusplus

/* Temporarily disabling BOLT on this function as we end up with a
 * comparison to the unoptimized function when referred to from a
 * function which isn't being BOLTed */
#define Ci_JIT_lazyJITInitFuncObjectVectorcall \
  Ci_JIT_lazyJITInitFuncObjectVectorcall_dont_bolt

PyObject* Ci_JIT_lazyJITInitFuncObjectVectorcall(
    PyFunctionObject* func,
    PyObject** stack,
    Py_ssize_t nargsf,
    PyObject* kwnames);

#ifdef __cplusplus
} // extern "C"
#endif
