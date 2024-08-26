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
 */
void scheduleJitCompile(PyFunctionObject* func);

/*
 * Check if a Python function entry point is a wrapper that will JIT-compile the
 * function in the future.
 *
 * Note: The compilation could happen in any number of future function calls,
 * it's determined by what the value of the AutoJIT threshold is.
 */
bool isJitEntryFunction(vectorcallfunc func);

/*
 * Get the appropriate entry point that will execute a function object in the
 * interpreter.
 *
 * This is a different function for Static Python functions versus "normal"
 * Python functions.
 */
vectorcallfunc getInterpretedVectorcall(PyFunctionObject* func);

/*
 * Specifies the offset from a JITed function entry point where the re-entry
 * point for calling with the correct bound args lives */
#define JITRT_CALL_REENTRY_OFFSET (-6)

/*
 * Fixes the JITed function entry point up to be the re-entry point after
 * binding the args */
#define JITRT_GET_REENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_CALL_REENTRY_OFFSET))

/*
 * Specifies the offset from a JITed function entry point where the static
 * entry point lives */
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define JITRT_STATIC_ENTRY_OFFSET (-11)
#else
/* Without JIT support there's no entry offset */
#define JITRT_STATIC_ENTRY_OFFSET (0)
#endif

/*
 * Fixes the JITed function entry point up to be the static entry point after
 * binding the args */
#define JITRT_GET_STATIC_ENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_STATIC_ENTRY_OFFSET))

/*
 * Fixes the JITed function entry point up to be the static entry point after
 * binding the args */
#define JITRT_GET_NORMAL_ENTRY_FROM_STATIC(entry) \
  ((vectorcallfunc)(((char*)entry) - JITRT_STATIC_ENTRY_OFFSET))

#ifdef __cplusplus
} // extern "C"
#endif
