/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <Python.h>

#include "cinderx/StaticPython/thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

// Defines a helper thunk with the same layout as our JIT generated static
// entry points.  This has the static entry point at the start, and then 11
// instructions in we have the vectorcall entry point.  We install the static
// entry points into the v-table and can easily switch to the vector call
// form when we're invoking from the interpreter or somewhere that can't use
// the native calling convention.
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define VTABLE_THUNK(name, _arg1_type)                                                \
  __attribute__((naked)) PyObject* name##_dont_bolt(                                  \
      _arg1_type* state, PyObject** args, size_t nargsf) {                            \
    __asm__(                                                                        \
                /* static_entry: */                                                 \
                /* we explicitly encode the jmp forward to static_entry_impl so */  \
                /* that we always get the 2 byte version.  The 0xEB is the jump, */ \
                /* the 10 is the length to jump, which is based upon the size of */ \
                /* jmp to the vectorcall entrypoint */                              \
                "push %rbp\n"                                                       \
                "mov %rsp, %rbp\n"                                                  \
                ".byte 0xEB\n"                                                      \
                ".byte 10\n"                                                        \
                "nop\n"                                                             \
                "nop\n"                                                             \
                "nop\n"                                                             \
                "nop\n"                                                             \
                "nop\n"                                                             \
                                                                                    \
                /* vector_entry: */                                                 \
                "jmp " #name "_vectorcall\n"                                        \
                                                                                    \
                /* static_entry_impl: */                                            \
                "push %rsp\n"                                                       \
                /* We want to push the arguments passed natively onto the stack */  \
                /* so that we can recover them in hydrate_args.  So we push them */ \
                /* onto the stack and then move the address of them into rsi */     \
                /* which will make them available as the 2nd argument.  Note we */  \
                /* don't need to push rdi as it's the state argument which we're */ \
                /* passing in anyway */                                             \
                "push %r9\n"                                                        \
                "push %r8\n"                                                        \
                "push %rcx\n"                                                       \
                "push %rdx\n"                                                       \
                "push %rsi\n"                                                       \
                "mov %rsp, %rsi\n"                                                  \
                "call " #name "_native\n"                                           \
                /* We don't know if we're returning a floating point value or not */\
                /* so we assume we are, and always populate the xmm registers */    \
                /* even if we don't need to */                                      \
                "movq %rax, %xmm0\n"                                                \
                "movq %rdx, %xmm1\n"                                                \
                "leave\n"                                                           \
                "ret\n"                                                             \
        ); \
  }
#else
#define VTABLE_THUNK(name, arg1_type)                     \
  PyObject* name##_dont_bolt(                             \
      arg1_type* state, PyObject** args, size_t nargsf) { \
    return name##_vectorcall(state, args, nargsf);        \
  }
#endif

typedef struct _PyClassLoader_StaticCallReturn {
  void* rax;
  void* rdx;
} _PyClassLoader_StaticCallReturn;

vectorcallfunc _PyClassLoader_GetStaticFunctionEntry(PyFunctionObject* func);

static const _PyClassLoader_StaticCallReturn StaticError = {0, 0};

// Converts native arguments that were pushed onto the stack into an
// array of PyObject** args that can be used to call into the interpreter.
int _PyClassLoader_HydrateArgs(
    PyCodeObject* code,
    Py_ssize_t arg_count,
    void** args,
    PyObject** call_args,
    PyObject** free_args);

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

PyObject* _PyVTable_coroutine_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);

PyObject* _PyVTable_func_lazyinit_vectorcall(
    _PyClassLoader_LazyFuncJitThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);
PyObject* _PyVTable_classmethod_overridable_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_coroutine_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_nonfunc_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_func_overridable_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_nonfunc_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_coroutine_classmethod_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf);
PyObject* _PyVTable_staticmethod_vectorcall(
    _PyClassLoader_StaticMethodThunk* method,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_staticmethod_overridable_dont_bolt(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_classmethod_dont_bolt(
    PyObject* state,
    PyObject** args,
    size_t nargsf);
PyObject* _PyVTable_thunk_vectorcall_only_dont_bolt(
    PyObject* state,
    PyObject** args,
    size_t nargsf);
PyObject*
_PyVTable_descr_dont_bolt(PyObject* state, PyObject** args, size_t nargsf);
PyObject* _PyVTable_func_missing_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    Py_ssize_t nargsf);

PyObject* _PyVTable_thunk_dont_bolt(
    _PyClassLoader_MethodThunk* state,
    PyObject** args,
    size_t nargsf);

#ifdef __cplusplus
}
#endif
