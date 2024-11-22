// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * Note: This header only exists to break a dependency cycle when compiling the
 * StaticPython/ and Jit/ directories.
 */

#pragma once

#include <Python.h>

#include "cinderx/Jit/pyjit_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Checks if the given function is JITed.
 *
 * Returns 1 if the function is JITed, 0 if not.
 */
int _PyJIT_IsCompiled(PyFunctionObject* func);

#ifdef __cplusplus
} // extern "C"
#endif
