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
