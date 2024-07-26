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
 * Bytecode definitions that do not exist in 3.12.  These should never match any
 * of the opcodes we actually read from .pyc files or otherwise fetch from the
 * runtime.
 */
#define OPCODE_DEFS(X)                            \
  X(BINARY_ADD)                                   \
  X(BINARY_AND)                                   \
  X(BINARY_FLOOR_DIVIDE)                          \
  X(BINARY_LSHIFT)                                \
  X(BINARY_MATRIX_MULTIPLY)                       \
  X(BINARY_MODULO)                                \
  X(BINARY_MULTIPLY)                              \
  X(BINARY_OR)                                    \
  X(BINARY_POWER)                                 \
  X(BINARY_RSHIFT)                                \
  X(BINARY_SUBSCR_DICT_STR)                       \
  X(BINARY_SUBSCR_LIST)                           \
  X(BINARY_SUBSCR_TUPLE)                          \
  X(BINARY_SUBSCR_TUPLE_CONST_INT)                \
  X(BINARY_SUBTRACT)                              \
  X(BINARY_TRUE_DIVIDE)                           \
  X(BINARY_XOR)                                   \
  X(BUILD_CHECKED_LIST)                           \
  X(BUILD_CHECKED_MAP)                            \
  X(CALL_FUNCTION)                                \
  X(CALL_FUNCTION_KW)                             \
  X(CALL_METHOD)                                  \
  X(CAST)                                         \
  X(CONVERT_PRIMITIVE)                            \
  X(COPY_DICT_WITHOUT_KEYS)                       \
  X(DUP_TOP)                                      \
  X(DUP_TOP_TWO)                                  \
  X(FAST_LEN)                                     \
  X(GEN_START)                                    \
  X(INPLACE_ADD)                                  \
  X(INPLACE_AND)                                  \
  X(INPLACE_FLOOR_DIVIDE)                         \
  X(INPLACE_LSHIFT)                               \
  X(INPLACE_MATRIX_MULTIPLY)                      \
  X(INPLACE_MODULO)                               \
  X(INPLACE_MULTIPLY)                             \
  X(INPLACE_OR)                                   \
  X(INPLACE_POWER)                                \
  X(INPLACE_RSHIFT)                               \
  X(INPLACE_SUBTRACT)                             \
  X(INPLACE_TRUE_DIVIDE)                          \
  X(INPLACE_XOR)                                  \
  X(INVOKE_FUNCTION)                              \
  X(INVOKE_METHOD)                                \
  X(INVOKE_NATIVE)                                \
  X(JUMP_ABSOLUTE)                                \
  X(JUMP_IF_FALSE_OR_POP)                         \
  X(JUMP_IF_NONZERO_OR_POP)                       \
  X(JUMP_IF_NOT_EXC_MATCH)                        \
  X(JUMP_IF_TRUE_OR_POP)                          \
  X(JUMP_IF_ZERO_OR_POP)                          \
  X(LIST_TO_TUPLE)                                \
  X(LOAD_ATTR_DICT_DESCR)                         \
  X(LOAD_ATTR_DICT_NO_DESCR)                      \
  X(LOAD_ATTR_NO_DICT_DESCR)                      \
  X(LOAD_ATTR_POLYMORPHIC)                        \
  X(LOAD_ATTR_SPLIT_DICT)                         \
  X(LOAD_ATTR_SPLIT_DICT_DESCR)                   \
  X(LOAD_ATTR_SUPER)                              \
  X(LOAD_ATTR_S_MODULE)                           \
  X(LOAD_ATTR_TYPE)                               \
  X(LOAD_ATTR_UNCACHABLE)                         \
  X(LOAD_CLASS)                                   \
  X(LOAD_FIELD)                                   \
  X(LOAD_ITERABLE_ARG)                            \
  X(LOAD_LOCAL)                                   \
  X(LOAD_METHOD_DICT_DESCR)                       \
  X(LOAD_METHOD_DICT_METHOD)                      \
  X(LOAD_METHOD_MODULE)                           \
  X(LOAD_METHOD_NO_DICT_DESCR)                    \
  X(LOAD_METHOD_NO_DICT_METHOD)                   \
  X(LOAD_METHOD_SPLIT_DICT_DESCR)                 \
  X(LOAD_METHOD_SPLIT_DICT_METHOD)                \
  X(LOAD_METHOD_SUPER)                            \
  X(LOAD_METHOD_S_MODULE)                         \
  X(LOAD_METHOD_TYPE)                             \
  X(LOAD_METHOD_TYPE_METHODLIKE)                  \
  X(LOAD_METHOD_UNCACHABLE)                       \
  X(LOAD_METHOD_UNSHADOWED_METHOD)                \
  X(LOAD_PRIMITIVE_FIELD)                         \
  X(LOAD_TYPE)                                    \
  X(MAKE_OPNAME)                                  \
  X(POP_JUMP_IF_NONZERO)                          \
  X(POP_JUMP_IF_ZERO)                             \
  X(PRIMITIVE_BINARY_OP)                          \
  X(PRIMITIVE_BOX)                                \
  X(PRIMITIVE_COMPARE_OP)                         \
  X(PRIMITIVE_LOAD_CONST)                         \
  X(PRIMITIVE_UNARY_OP)                           \
  X(PRIMITIVE_UNBOX)                              \
  X(REFINE_TYPE)                                  \
  X(RETURN_PRIMITIVE)                             \
  X(ROT_FOUR)                                     \
  X(ROT_N)                                        \
  X(ROT_THREE)                                    \
  X(ROT_TWO)                                      \
  X(SEQUENCE_GET)                                 \
  X(SEQUENCE_SET)                                 \
  X(SETUP_ASYNC_WITH)                             \
  X(STORE_ATTR_DESCR)                             \
  X(STORE_ATTR_DICT)                              \
  X(STORE_ATTR_SPLIT_DICT)                        \
  X(STORE_ATTR_UNCACHABLE)                        \
  X(STORE_FIELD)                                  \
  X(STORE_LOCAL)                                  \
  X(STORE_PRIMITIVE_FIELD)                        \
  X(TP_ALLOC)                                     \
  X(UNARY_POSITIVE)                               \
  X(YIELD_FROM)                                   \

enum {
  /*
   * Uses magic value to put these in a completely bogus range that doesn't fit
   * in one byte.  Fits in two bytes which matches how CPython handles pseudo
   * opcodes.
   */
  STUB_OPCODE_BEGIN = 40000,
#define DEFINE_OPCODE(NAME) NAME,
  OPCODE_DEFS(DEFINE_OPCODE)
#undef DEFINE_OPCODE
};

/*
 * Intended to list out all the opcode definitions.  Used by the HIR printer.
 * No way of generating this in 3.12 right now.
 */
#define PY_OPCODES(X)

/*
 * GC
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

void _PyAwaitable_SetAwaiter(PyObject *receiver, PyObject *awaiter);
typedef void (*setawaiterfunc)(PyObject *receiver, PyObject *awaiter);
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

PyObject *
    Ci_Super_Lookup(PyTypeObject *type, PyObject *obj, PyObject *name,
                    PyObject *super_instance, int *meth_found);
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
PyDictKeysObject *_PyDict_MakeKeysShared(PyObject *op);
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

// Get the callable out of a classmethod object.
//
// TODO: This and Ci_PyStaticMethod_GetFunc() should be used for 3.10 as well.
static inline PyObject* Ci_PyClassMethod_GetFunc(PyObject* classmethod) {
  PyMemberDef* member = &Py_TYPE(classmethod)->tp_members[0];
  assert(strcmp(member->name, "__func__") == 0);
  Py_ssize_t offset = member->offset;
  return *((PyObject**)((char*)classmethod + offset));
}

// Get the callable out of a staticmethod object.
static inline PyObject* Ci_PyStaticMethod_GetFunc(PyObject* staticmethod) {
  // classmethod and staticmethod have the same underlying structure.
  return Ci_PyClassMethod_GetFunc(staticmethod);
}

PyObject* Ci_StaticFunction_Vectorcall(PyObject *func, PyObject* const* stack,
                       size_t nargsf, PyObject *kwnames);
PyObject* Ci_PyFunction_CallStatic(PyFunctionObject *func,
                       PyObject* const* args,
                       Py_ssize_t nargsf,
                       PyObject *kwnames);

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
