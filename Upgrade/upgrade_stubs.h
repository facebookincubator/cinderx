// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>
#if PY_VERSION_HEX >= 0x030C0000

#include <stdint.h>

#include "cinderx/Upgrade/upgrade_assert.h"  // @donotremove

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Intended to list out all the opcode definitions.  Used by the HIR printer.
 * No way of generating this in 3.12 right now.
 */
#define PY_OPCODES(X)

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
typedef Py_ssize_t (*Ci_gc_collect_t)(struct Ci_PyGCImpl *impl, PyThreadState* tstate, int generation,
                                      Py_ssize_t *n_collected, Py_ssize_t *n_uncollectable,
                                      int nofail);

// Free a collector
typedef void (*Ci_gc_finalize_t)(struct Ci_PyGCImpl *impl);

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
 * Returns a pointer to the previous impl, which the caller is responsible for freeing
 * using the returned impl's finalize().
 */
Ci_PyGCImpl* Ci_PyGC_SetImpl(struct _gc_runtime_state *gc_state, Ci_PyGCImpl *impl);

/*
 * Returns a pointer to the current GC implementation but does not transfer
 * ownership to the caller.
 */
Ci_PyGCImpl* Ci_PyGC_GetImpl(struct _gc_runtime_state *gc_state);

/*
 * Clear free lists (e.g. frames, tuples, etc.) for the given interpreter.
 *
 * This should be called by GC implementations after collecting the highest
 * generation.
 */
void Ci_PyGC_ClearFreeLists(PyInterpreterState *interp);

/*
 * Generators
 */


/* Enum shared with the JIT to communicate the current state of a generator.
 * These should be queried via the utility functions below. These may use some
 * other features to determine the overall state of a JIT generator. Notably
 * the yield-point field being null indicates execution is currently active when
 * used in combination with the less specific Ci_JITGenState_Running state.
 */
typedef enum {
  /* Generator has freshly been returned from a call to the function itself.
     Execution of user code has not yet begun. */
  Ci_JITGenState_JustStarted,
  /* Execution is in progress and is currently active or the generator is
     suspended. */
  Ci_JITGenState_Running,
  /* Generator has completed execution and should not be resumed again. */
  Ci_JITGenState_Completed,
  /* An exception/close request is being processed.  */
  Ci_JITGenState_Throwing,
} CiJITGenState;

int Ci_GenIsCompleted(PyGenObject *gen);
int Ci_GenIsExecuting(PyGenObject *gen);
CiJITGenState Ci_GetJITGenState(PyGenObject *gen);
int Ci_JITGenIsExecuting(PyGenObject *gen);

PyObject* CiCoro_New_NoFrame(PyThreadState *tstate, PyCodeObject *code);
PyObject* CiAsyncGen_New_NoFrame(PyCodeObject *code);
PyObject* CiGen_New_NoFrame(PyCodeObject *code);

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
    PyObject *wh_coro_or_result;
    PyObject *wh_waiter;
} Ci_PyWaitHandleObject;

/*
 * Awaited flag
 */
int Ci_PyWaitHandle_CheckExact(PyObject* obj);
void Ci_PyWaitHandle_Release(PyObject *wait_handle);

/*
 * Shadow frames.
 */

typedef struct _PyShadowFrame {
  struct _PyShadowFrame *prev;
  uintptr_t data;
} _PyShadowFrame;

typedef struct JITShadowFrame {
  _PyShadowFrame sf;
  uintptr_t orig_data;
} JITShadowFrame;

typedef enum {
  PYSF_CODE_RT = 0b00,
  PYSF_PYFRAME = 0b01,
  PYSF_RTFS = 0b10,
  PYSF_DUMMY = 0b11,
} _PyShadowFrame_PtrKind;

typedef enum {
  PYSF_JIT = 0,
  PYSF_INTERP = 1,
} _PyShadowFrame_Owner;

static const unsigned int _PyShadowFrame_NumTagBits = 4;
static const uintptr_t _PyShadowFrame_TagMask =
    (1 << _PyShadowFrame_NumTagBits) - 1;
static const uintptr_t _PyShadowFrame_PtrMask = ~_PyShadowFrame_TagMask;

#define TAG_MASK(num_bits, off) ((1 << num_bits) - 1) << off

static const unsigned int kShadowFrameSize = sizeof(_PyShadowFrame);
#define SHADOW_FRAME_FIELD_OFF(field) (int{(offsetof(_PyShadowFrame, field))})
static const unsigned int kJITShadowFrameSize = sizeof(JITShadowFrame);
#define JIT_SHADOW_FRAME_FIELD_OFF(field)                                      \
  (int{(offsetof(JITShadowFrame, field))})
static const unsigned int _PyShadowFrame_NumPtrKindBits = 2;
static const unsigned int _PyShadowFrame_PtrKindOff = 0;
static const uintptr_t _PyShadowFrame_PtrKindMask =
    TAG_MASK(_PyShadowFrame_NumPtrKindBits, _PyShadowFrame_PtrKindOff);

static const unsigned int _PyShadowFrame_NumOwnerBits = 1;
static const unsigned int _PyShadowFrame_OwnerOff =
    _PyShadowFrame_PtrKindOff + _PyShadowFrame_NumPtrKindBits;
static const uintptr_t _PyShadowFrame_OwnerMask =
    TAG_MASK(_PyShadowFrame_NumOwnerBits, _PyShadowFrame_OwnerOff);

#undef TAG_MASK

uintptr_t _PyShadowFrame_MakeData(void *ptr,
                                                _PyShadowFrame_PtrKind ptr_kind,
                                                _PyShadowFrame_Owner owner);

void _PyShadowFrame_SetOwner(_PyShadowFrame *shadow_frame,
                                           _PyShadowFrame_Owner owner);

void _PyShadowFrame_Pop(PyThreadState *tstate,
                                      _PyShadowFrame *shadow_frame);

_PyShadowFrame_PtrKind _PyShadowFrame_GetPtrKind(_PyShadowFrame *shadow_frame);
_PyShadowFrame_Owner _PyShadowFrame_GetOwner(_PyShadowFrame *shadow_frame);
PyGenObject* _PyShadowFrame_GetGen(_PyShadowFrame *shadow_frame);
_PyShadowFrame_PtrKind JITShadowFrame_GetRTPtrKind(JITShadowFrame *jit_sf);
void* JITShadowFrame_GetRTPtr(JITShadowFrame *jit_sf);
PyFrameObject *_PyShadowFrame_GetPyFrame(_PyShadowFrame *shadow_frame);
PyCodeObject* _PyShadowFrame_GetCode(_PyShadowFrame* shadow_frame);
PyObject* _PyShadowFrame_GetFullyQualifiedName(_PyShadowFrame* shadow_frame);

/*
 * Interpreter exports
 */


int Cix_eval_frame_handle_pending(PyThreadState *tstate);
PyObject *
    Cix_special_lookup(PyThreadState *tstate, PyObject *o, _Py_Identifier *id);
void Cix_format_kwargs_error(PyThreadState *tstate, PyObject *func,
                                         PyObject *kwargs);
void
    Cix_format_awaitable_error(PyThreadState *tstate, PyTypeObject *type,
                               int prevprevopcode, int prevopcode);
PyFrameObject *
    Cix_PyEval_MakeFrameVector(PyThreadState *tstate, PyFrameConstructor *con,
                               PyObject *locals, PyObject *const *args,
                               Py_ssize_t argcount, PyObject *kwnames);
PyObject *
    Cix_SuperLookupMethodOrAttr(PyThreadState *tstate, PyObject *global_super,
                                PyTypeObject *type, PyObject *self,
                                PyObject *name, int call_no_args,
                                int *meth_found);
int
    Cix_do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);
void Cix_format_exc_check_arg(PyThreadState *, PyObject *,
                                          const char *, PyObject *);
PyObject *
    Cix_match_class(PyThreadState *tstate, PyObject *subject, PyObject *type,
                    Py_ssize_t nargs, PyObject *kwargs);
PyObject *
    Cix_match_keys(PyThreadState *tstate, PyObject *map, PyObject *keys);

int Cix_cfunction_check_kwargs(PyThreadState *tstate, PyObject *func, PyObject *kwnames);
typedef void (*funcptr)(void);
funcptr Cix_cfunction_enter_call(PyThreadState *tstate, PyObject *func);
funcptr Cix_method_enter_call(PyThreadState *tstate, PyObject *func);

// Implementation in Python/bltinmodule.c
PyObject * builtin_next(PyObject *self, PyObject *const *args, Py_ssize_t nargs);
PyObject * Ci_Builtin_Next_Core(PyObject *it, PyObject *def);

/*
 * Dicts/Objects
 */

int _PyDict_HasUnsafeKeys(PyObject *dict);
int _PyDict_HasOnlyUnicodeKeys(PyObject *);
Py_ssize_t _PyDictKeys_GetSplitIndex(PyDictKeysObject *keys, PyObject *key);
PyObject** Ci_PyObject_GetDictPtrAtOffset(PyObject *obj, Py_ssize_t dictoffset);
PyObject* _PyDict_GetItem_Unicode(PyObject *op, PyObject *key);
PyObject* _PyDict_GetItem_UnicodeExact(PyObject *op, PyObject *key);
int PyDict_NextKeepLazy(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue);


/*
 * Custom Cinder stack walking
 */

typedef enum {
  CI_SWD_STOP_STACK_WALK = 0,
  CI_SWD_CONTINUE_STACK_WALK = 1,
} CiStackWalkDirective;

typedef CiStackWalkDirective (*CiWalkStackCallback)(void *data,
                                                    PyCodeObject *code,
                                                    int lineno);

typedef CiStackWalkDirective (*CiWalkAsyncStackCallback)(void *data,
                                                    PyObject *fqname,
                                                    PyCodeObject *code,
                                                    int lineno,
                                                    PyObject* pyFrame);

/*
 * Misc
 */

int Ci_set_attribute_error_context(PyObject *v, PyObject *name);
void _PyType_ClearNoShadowingInstances(struct _typeobject *, PyObject *obj);
int PyUnstable_PerfTrampoline_CompileCode(PyCodeObject *);
int PyUnstable_PerfTrampoline_SetPersistAfterFork(int enable);
PyObject* _PyObject_GC_Malloc(size_t size);

#define _Py_IDENTIFIER(name) \
    UPGRADE_ASSERT("_Py_IDENTIFIER(" #name ")") \
    static _Py_Identifier PyId_##name;

#define _Py_static_string(name, str) \
    UPGRADE_ASSERT("_Py_static_string(" #name ", " #str ")") \
    static _Py_Identifier name;

// Ideally this would live in cinderx/Common/dict.h, but it can't right now
// because that has conflicts when pulled into checked_dict.c
// (e.g. redefines _dictkeysobject).
#define _PyDict_NotifyEvent(EVENT, MP, KEY, VAL) \
  _PyDict_NotifyEvent(_PyInterpreterState_GET(), (EVENT), (MP), (KEY), (VAL))

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PY_VERSION_HEX >= 0x030C0000
