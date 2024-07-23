// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Jit/pyjit_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JIT compile func and patch its entry point.
 *
 * On success, positional only calls to func will use the JIT compiled version.
 *
 * Returns PYJIT_RESULT_OK on success.
 */
PyAPI_FUNC(_PyJIT_Result) _PyJIT_CompileFunction(PyFunctionObject* func);

/*
 * Registers a function with the JIT to be compiled in the future.
 *
 * The JIT will still be informed by _PyJIT_CompileFunction before the
 * function executes for the first time.  The JIT can choose to compile
 * the function at some future point.  Currently the JIT will compile
 * the function before it shuts down to make sure all eligable functions
 * were compiled.
 *
 * The JIT will not keep the function alive, instead it will be informed
 * that the function is being de-allocated via _PyJIT_UnregisterFunction
 * before the function goes away.
 *
 * Returns 1 if the function is registered with JIT or is already compiled,
 * and 0 otherwise.
 */
PyAPI_FUNC(int) _PyJIT_RegisterFunction(PyFunctionObject* func);

/*
 * Checks if the given function is JITed.

 * Returns 1 if the function is JITed, 0 if not.
 */
PyAPI_FUNC(int) _PyJIT_IsCompiled(PyFunctionObject* func);

#ifdef __cplusplus
} // extern "C"
#endif
