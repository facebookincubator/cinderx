// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Overwrite the entry point of a function so that it tries to JIT-compile
 * itself in the future.
 *
 * By default this will trigger the JIT the next time the function is called,
 * unless AutoJIT is enabled, in that case the function will compile after it is
 * called more times than the AutoJIT threshold.  Before that it will run
 * through the interpreter.
 *
 * Return true if the function was successfully scheduled for compilation, or if
 * it is already compiled.
 */
bool scheduleJitCompile(PyFunctionObject* func);

#ifdef __cplusplus
} // extern "C"
#endif
