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
 * Generators
 */

int Ci_GenIsCompleted(PyGenObject* gen);
int Ci_GenIsExecuting(PyGenObject* gen);
int Ci_JITGenIsExecuting(PyGenObject* gen);

PyObject* CiCoro_New_NoFrame(PyThreadState* tstate, PyCodeObject* code);
PyObject* CiAsyncGen_New_NoFrame(PyCodeObject* code);
PyObject* CiGen_New_NoFrame(PyCodeObject* code);

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

/*
 * Dicts/Objects
 */

int _PyDict_HasUnsafeKeys(PyObject* dict);
Py_ssize_t _PyDictKeys_GetSplitIndex(PyDictKeysObject* keys, PyObject* key);

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
