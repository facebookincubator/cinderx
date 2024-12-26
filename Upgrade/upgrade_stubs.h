// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>
#if PY_VERSION_HEX >= 0x030C0000

#include "cinderx/Upgrade/upgrade_assert.h" // @donotremove

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parallel GC stuff T194027914.
 */

struct Ci_PyGCImpl;

/*
 * Collect cyclic garbage.
 *
 * impl           - Pointer to the collection implementation.
 * tstate         - Indirectly specifies (via tstate->interp) the interpreter
                    for which collection should be performed.
 * generation     - Collect generations <= this value
 * n_collected    - Out param for number of objects collected
 * n_unollectable - Out param for number of uncollectable garbage objects
 * nofail         - When true, swallow exceptions that occur during collection
 */
typedef Py_ssize_t (*Ci_gc_collect_t)(
    struct Ci_PyGCImpl* impl,
    PyThreadState* tstate,
    int generation,
    Py_ssize_t* n_collected,
    Py_ssize_t* n_uncollectable,
    int nofail);

// Free a collector
typedef void (*Ci_gc_finalize_t)(struct Ci_PyGCImpl* impl);

// An implementation of cyclic garbage collection
typedef struct Ci_PyGCImpl {
  Ci_gc_collect_t collect;
  Ci_gc_finalize_t finalize;
} Ci_PyGCImpl;

struct _gc_runtime_state;

/*
 * Set the collection implementation.
 *
 * The callee takes ownership of impl.
 *
 * Returns a pointer to the previous impl, which the caller is responsible for
 * freeing using the returned impl's finalize().
 */
Ci_PyGCImpl* Ci_PyGC_SetImpl(
    struct _gc_runtime_state* gc_state,
    Ci_PyGCImpl* impl);

/*
 * Returns a pointer to the current GC implementation but does not transfer
 * ownership to the caller.
 */
Ci_PyGCImpl* Ci_PyGC_GetImpl(struct _gc_runtime_state* gc_state);

/*
 * Clear free lists (e.g. frames, tuples, etc.) for the given interpreter.
 *
 * This should be called by GC implementations after collecting the highest
 * generation.
 */
void Ci_PyGC_ClearFreeLists(PyInterpreterState* interp);

/*
 * Generators
 */

int Ci_GenIsCompleted(PyGenObject* gen);
int Ci_GenIsExecuting(PyGenObject* gen);
int Ci_JITGenIsExecuting(PyGenObject* gen);

PyObject* CiCoro_New_NoFrame(PyThreadState* tstate, PyCodeObject* code);
PyObject* CiAsyncGen_New_NoFrame(PyCodeObject* code);
PyObject* CiGen_New_NoFrame(PyCodeObject* code);

typedef void (*setawaiterfunc)(PyObject* receiver, PyObject* awaiter);

typedef void (*setawaiterfunc)(PyObject* receiver, PyObject* awaiter);
typedef struct {
  PyAsyncMethods ame_async_methods;
  sendfunc ame_send;
  setawaiterfunc ame_setawaiter;
} PyAsyncMethodsWithExtra;

/* Offsets for fields in jit::GenFooterData for fast access from C code.
 * These values are verified by static_assert in runtime.h. */
#define Ci_GEN_JIT_DATA_OFFSET_STATE 32
#define Ci_GEN_JIT_DATA_OFFSET_YIELD_POINT 24

typedef struct {
  PyObject_HEAD
  PyObject* wh_coro_or_result;
  PyObject* wh_waiter;
} Ci_PyWaitHandleObject;

/*
 * Awaited flag
 */
int Ci_PyWaitHandle_CheckExact(PyObject* obj);
void Ci_PyWaitHandle_Release(PyObject* wait_handle);

/*
 * Interpreter exports
 */
int Cix_cfunction_check_kwargs(
    PyThreadState* tstate,
    PyObject* func,
    PyObject* kwnames);
typedef void (*funcptr)(void);
funcptr Cix_cfunction_enter_call(PyThreadState* tstate, PyObject* func);
funcptr Cix_method_enter_call(PyThreadState* tstate, PyObject* func);

// Implementation in Python/bltinmodule.c
PyObject* builtin_next(PyObject* self, PyObject* const* args, Py_ssize_t nargs);
PyObject* Ci_Builtin_Next_Core(PyObject* it, PyObject* def);

/*
 * Dicts/Objects
 */

int _PyDict_HasUnsafeKeys(PyObject* dict);
Py_ssize_t _PyDictKeys_GetSplitIndex(PyDictKeysObject* keys, PyObject* key);
int PyDict_NextKeepLazy(
    PyObject* op,
    Py_ssize_t* ppos,
    PyObject** pkey,
    PyObject** pvalue);

/*
 * Custom Cinder stack walking
 */

typedef enum {
  CI_SWD_STOP_STACK_WALK = 0,
  CI_SWD_CONTINUE_STACK_WALK = 1,
} CiStackWalkDirective;

typedef CiStackWalkDirective (
    *CiWalkStackCallback)(void* data, PyCodeObject* code, int lineno);

typedef CiStackWalkDirective (*CiWalkAsyncStackCallback)(
    void* data,
    PyObject* fqname,
    PyCodeObject* code,
    int lineno,
    PyObject* pyFrame);

/*
 * Misc
 */

void _PyType_ClearNoShadowingInstances(struct _typeobject*, PyObject* obj);

#define _Py_IDENTIFIER(name)                  \
  UPGRADE_ASSERT("_Py_IDENTIFIER(" #name ")") \
  static _Py_Identifier PyId_##name;

#define _Py_static_string(name, str)                       \
  UPGRADE_ASSERT("_Py_static_string(" #name ", " #str ")") \
  static _Py_Identifier name;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PY_VERSION_HEX >= 0x030C0000
