// Copyright (c) Meta Platforms, Inc. and affiliates.

// clang-format off

#define CINDERX_INTERPRETER

#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/Interpreter/cinder_opcode.h"

#include "cinderx/module_c_state.h"

#include "cinderx/Common/code.h"

//#include "internal/pycore_opcode.h"

// Must come after pycore_opcode, we want to get the exported ones.
#define NEED_OPCODE_NAMES
#define NEED_OPCODE_TABLES
#include "cinderx/Interpreter/cinder_opcode.h"

#include "internal/pycore_ceval.h"
#include "internal/pycore_stackref.h"
#include "internal/pycore_interpframe.h"

#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/static_array.h"

#include "cinderx/Jit/generators_rt.h"

#ifdef ENABLE_INTERPRETER_LOOP

bool Ci_DelayAdaptiveCode = false;
uint64_t Ci_AdaptiveThreshold = 80;

bool is_adaptive_enabled(CodeExtra *extra) {
    return !Ci_DelayAdaptiveCode || extra->calls > Ci_AdaptiveThreshold;
}

#endif

/* _PyEval_EvalFrameDefault() is a *big* function,
 * so consume 3 units of C stack */
#define PY_EVAL_C_STACK_UNITS 2

// These are used to truncate primitives/check signed bits when converting
// between them


#ifdef ENABLE_INTERPRETER_LOOP

static uint64_t trunc_masks[] = {0xFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF};
static uint64_t signed_bits[] = {0x80, 0x8000, 0x80000000, 0x8000000000000000};
static uint64_t signex_masks[] = {
    0xFFFFFFFFFFFFFF00,
    0xFFFFFFFFFFFF0000,
    0xFFFFFFFF00000000,
    0x0};

static int8_t unbox_primitive_bool(PyObject* x) {
    assert(PyBool_Check(x));
    return (x == Py_True) ? 1 : 0;
}

static Py_ssize_t unbox_primitive_int(PyObject* x) {
    assert(PyLong_Check(x));
    return (Py_ssize_t)PyLong_AsVoidPtr(x);
}

static PyObject* box_primitive(int type, Py_ssize_t value) {
  switch (type) {
    case TYPED_BOOL:
      return PyBool_FromLong((int8_t)value);
    case TYPED_INT8:
    case TYPED_CHAR:
      return PyLong_FromSsize_t((int8_t)value);
    case TYPED_INT16:
      return PyLong_FromSsize_t((int16_t)value);
    case TYPED_INT32:
      return PyLong_FromSsize_t((int32_t)value);
    case TYPED_INT64:
      return PyLong_FromSsize_t((int64_t)value);
    case TYPED_UINT8:
      return PyLong_FromSize_t((uint8_t)value);
    case TYPED_UINT16:
      return PyLong_FromSize_t((uint16_t)value);
    case TYPED_UINT32:
      return PyLong_FromSize_t((uint32_t)value);
    case TYPED_UINT64:
      return PyLong_FromSize_t((uint64_t)value);
    default:
      assert(0);
      return NULL;
  }
}

static _PyStackRef sign_extend_primitive(_PyStackRef obj, int type) {
    if ((type & (TYPED_INT_SIGNED)) && type != (TYPED_DOUBLE)) {
        /* We have a boxed value on the stack already, but we may have to
        * deal with sign extension */
        PyObject* val = PyStackRef_AsPyObjectBorrow(obj);
        size_t ival = (size_t)PyLong_AsVoidPtr(val);
        if (ival & ((size_t)1) << 63) {
            PyStackRef_CLOSE(obj);
            return PyStackRef_FromPyObjectSteal(PyLong_FromSsize_t((int64_t)ival));
        }
    }
    return obj;
}

static PyObject* load_field(int field_type, void* addr) {
    PyObject* value;
    switch (field_type) {
        case TYPED_BOOL:
            value = PyBool_FromLong(*(int8_t*)addr);
            break;
        case TYPED_INT8:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((int8_t*)addr));
            break;
        case TYPED_INT16:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((int16_t*)addr));
            break;
        case TYPED_INT32:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((int32_t*)addr));
            break;
        case TYPED_INT64:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((int64_t*)addr));
            break;
        case TYPED_UINT8:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((uint8_t*)addr));
            break;
        case TYPED_UINT16:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((uint16_t*)addr));
            break;
        case TYPED_UINT32:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((uint32_t*)addr));
            break;
        case TYPED_UINT64:
            value = PyLong_FromVoidPtr((void*)(Py_ssize_t) * ((uint64_t*)addr));
            break;
        case TYPED_DOUBLE:
            value = PyFloat_FromDouble(*(double*)addr);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "unsupported field type");
            return NULL;
    }
    return value;
}

static void store_field(int field_type, void* addr, PyObject* value) {
    switch (field_type) {
        case TYPED_BOOL:
            *(int8_t*)addr = (int8_t)unbox_primitive_bool(value);
            break;
        case TYPED_INT8:
            *(int8_t*)addr = (int8_t)unbox_primitive_int(value);
            break;
        case TYPED_INT16:
            *(int16_t*)addr = (int16_t)unbox_primitive_int(value);
            break;
        case TYPED_INT32:
            *(int32_t*)addr = (int32_t)unbox_primitive_int(value);
            break;
        case TYPED_INT64:
            *(int64_t*)addr = (int64_t)unbox_primitive_int(value);
            break;
        case TYPED_UINT8:
            *(uint8_t*)addr = (uint8_t)unbox_primitive_int(value);
            break;
        case TYPED_UINT16:
            *(uint16_t*)addr = (uint16_t)unbox_primitive_int(value);
            break;
        case TYPED_UINT32:
            *(uint32_t*)addr = (uint32_t)unbox_primitive_int(value);
            break;
        case TYPED_UINT64:
            *(uint64_t*)addr = (uint64_t)unbox_primitive_int(value);
            break;
        case TYPED_DOUBLE:
            *((double*)addr) = PyFloat_AsDouble(value);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "unsupported field type");
            break;
    }
}

#define FIELD_OFFSET(self, offset) (PyObject**)(((char*)self) + offset)

static int ci_build_dict(_PyStackRef *map_items, Py_ssize_t map_size, PyObject *map)
{
    for (Py_ssize_t i = 0; i < map_size; i++) {
        PyObject* key = PyStackRef_AsPyObjectBorrow(map_items[2 * i]);
        PyObject* value = PyStackRef_AsPyObjectBorrow(map_items[2 * i + 1]);
        if (Ci_CheckedDict_SetItem(map, key, value) < 0) {
            return -1;
        }
    }
    return 0;
}

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
static void specialize_with_value(_Py_CODEUNIT next_instr, PyObject *value, int opcode,
                                  int shift, int bits)
{
    int32_t index = _PyClassLoader_CacheValue(value);
    if (index >= 0 && index <= (INT32_MAX >> 2)) {
        int32_t *cache = (int32_t*)next_instr;
        *cache = (int32_t)(index << shift) | bits;
        _Ci_specialize(next_instr, opcode);
    }
}
#endif

#define INT_UNARY_OPCODE(opid, op)                                           \
    case opid:                                                               \
        res = PyLong_FromVoidPtr((void*)(op(size_t) PyLong_AsVoidPtr(val))); \
        break;

#define DBL_UNARY_OPCODE(opid, op)                          \
    case opid:                                              \
      res = PyFloat_FromDouble(op(PyFloat_AS_DOUBLE(val))); \
      break;

static PyObject *
primitive_unary_op(PyObject *val, int oparg)
{
    PyObject *res;
    switch (oparg) {
        INT_UNARY_OPCODE(PRIM_OP_NEG_INT, -)
        INT_UNARY_OPCODE(PRIM_OP_INV_INT, ~)
        DBL_UNARY_OPCODE(PRIM_OP_NEG_DBL, -)
        case PRIM_OP_NOT_INT: {
            res = PyLong_AsVoidPtr(val) ? Py_False : Py_True;
            Py_INCREF(res);
            break;
        }
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown op");
            return NULL;
    }
    return res;
}

#define INT_BIN_OPCODE_UNSIGNED(opid, op)                                 \
    case opid:                                                            \
        res = PyLong_FromVoidPtr((void*)(((size_t)PyLong_AsVoidPtr(l))op( \
            (size_t)PyLong_AsVoidPtr(r))));                               \
        break;

#define INT_BIN_OPCODE_SIGNED(opid, op)                                       \
    case opid:                                                                \
        res = PyLong_FromVoidPtr((void*)(((Py_ssize_t)PyLong_AsVoidPtr(l))op( \
            (Py_ssize_t)PyLong_AsVoidPtr(r))));                               \
        break;

#define DOUBLE_BIN_OPCODE(opid, op)                                                 \
    case opid:                                                                      \
        res = (PyFloat_FromDouble((PyFloat_AS_DOUBLE(l))op(PyFloat_AS_DOUBLE(r)))); \
        break;

static PyObject *
primitive_binary_op(PyObject *l, PyObject *r, int oparg)
{
    PyObject *res;
    switch (oparg) {
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_ADD_INT, +)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_SUB_INT, -)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_MUL_INT, *)
        INT_BIN_OPCODE_SIGNED(PRIM_OP_DIV_INT, /)
        INT_BIN_OPCODE_SIGNED(PRIM_OP_MOD_INT, %)
        case PRIM_OP_POW_INT: {
            double power =
                pow((Py_ssize_t)PyLong_AsVoidPtr(l),
                    (Py_ssize_t)PyLong_AsVoidPtr(r));
            res = PyFloat_FromDouble(power);
            break;
        }
        case PRIM_OP_POW_UN_INT: {
            double power =
                pow((size_t)PyLong_AsVoidPtr(l), (size_t)PyLong_AsVoidPtr(r));
            res = PyFloat_FromDouble(power);
            break;
        }

        INT_BIN_OPCODE_SIGNED(PRIM_OP_LSHIFT_INT, <<)
        INT_BIN_OPCODE_SIGNED(PRIM_OP_RSHIFT_INT, >>)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_XOR_INT, ^)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_OR_INT, |)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_AND_INT, &)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_MOD_UN_INT, %)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_DIV_UN_INT, /)
        INT_BIN_OPCODE_UNSIGNED(PRIM_OP_RSHIFT_UN_INT, >>)
        DOUBLE_BIN_OPCODE(PRIM_OP_ADD_DBL, +)
        DOUBLE_BIN_OPCODE(PRIM_OP_SUB_DBL, -)
        DOUBLE_BIN_OPCODE(PRIM_OP_MUL_DBL, *)
        DOUBLE_BIN_OPCODE(PRIM_OP_DIV_DBL, /)
        case PRIM_OP_POW_DBL: {
            double power = pow(PyFloat_AsDouble(l), PyFloat_AsDouble(r));
            res = PyFloat_FromDouble(power);
            break;
        }
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown op");
            return NULL;
    }
    return res;
}

#define INT_CMP_OPCODE_UNSIGNED(opid, op)           \
    case opid:                                      \
        right = (size_t)PyLong_AsVoidPtr(r);        \
        left = (size_t)PyLong_AsVoidPtr(l);         \
        res = (left op right) ? Py_True : Py_False; \
        Py_INCREF(res);                             \
        break;

#define INT_CMP_OPCODE_SIGNED(opid, op)               \
    case opid:                                        \
        sright = (Py_ssize_t)PyLong_AsVoidPtr(r);     \
        sleft = (Py_ssize_t)PyLong_AsVoidPtr(l);      \
        res = (sleft op sright) ? Py_True : Py_False; \
        Py_INCREF(res);                               \
        break;

#define DBL_CMP_OPCODE(opid, op)                                                   \
    case opid:                                                                     \
        res =                                                                      \
            ((PyFloat_AS_DOUBLE(l) op PyFloat_AS_DOUBLE(r)) ? Py_True : Py_False); \
        Py_INCREF(res);                                                            \
        break;

static PyObject *
primitive_compare_op(PyObject *l, PyObject *r, int oparg)
{
    PyObject *res;
    Py_ssize_t sleft, sright;
    size_t left, right;
    switch (oparg) {
        INT_CMP_OPCODE_SIGNED(PRIM_OP_EQ_INT, ==)
        INT_CMP_OPCODE_SIGNED(PRIM_OP_NE_INT, !=)
        INT_CMP_OPCODE_SIGNED(PRIM_OP_LT_INT, <)
        INT_CMP_OPCODE_SIGNED(PRIM_OP_GT_INT, >)
        INT_CMP_OPCODE_SIGNED(PRIM_OP_LE_INT, <=)
        INT_CMP_OPCODE_SIGNED(PRIM_OP_GE_INT, >=)
        INT_CMP_OPCODE_UNSIGNED(PRIM_OP_LT_UN_INT, <)
        INT_CMP_OPCODE_UNSIGNED(PRIM_OP_GT_UN_INT, >)
        INT_CMP_OPCODE_UNSIGNED(PRIM_OP_LE_UN_INT, <=)
        INT_CMP_OPCODE_UNSIGNED(PRIM_OP_GE_UN_INT, >=)
        DBL_CMP_OPCODE(PRIM_OP_EQ_DBL, ==)
        DBL_CMP_OPCODE(PRIM_OP_NE_DBL, !=)
        DBL_CMP_OPCODE(PRIM_OP_LT_DBL, <)
        DBL_CMP_OPCODE(PRIM_OP_GT_DBL, >)
        DBL_CMP_OPCODE(PRIM_OP_LE_DBL, <=)
        DBL_CMP_OPCODE(PRIM_OP_GE_DBL, >=)
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown op");
            return NULL;
    }
    return res;
}

#define INVOKE_FUNCTION_CACHE_SIZE 4
#define TP_ALLOC_CACHE_SIZE 2
#define STORE_LOCAL_CACHE_SIZE 1
#define INLINE_CACHE_ENTRIES_LOAD_FIELD 2
#define INLINE_CACHE_ENTRIES_STORE_FIELD 2
#define CAST_CACHE_SIZE 2
#define INLINE_CACHE_ENTRIES_BUILD_CHECKED_LIST 2
#define INLINE_CACHE_ENTRIES_BUILD_CHECKED_MAP 2
#endif

#ifdef ENABLE_INTERPRETER_LOOP

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag);

#include "cinderx/Interpreter/3.15/ceval.h"
#include "Python/ceval.h"

#endif

#define _PyEval_GetAwaitable Ci_PyEval_GetAwaitable
#define _PyEval_GetANext Ci_PyEval_GetANext

void Ci_InitOpcodes() {
#ifdef ENABLE_ADAPTIVE_STATIC_PYTHON
    // patch CPython's opcode data
    for (int i = 0; i < sizeof(_CiOpcode_Caches) / sizeof(_CiOpcode_Caches[0]); i++) {
        _PyOpcode_Caches[i] = _CiOpcode_Caches[i];
    }
    for (int i = 0; i < sizeof(_CiOpcode_Deopt) / sizeof(_CiOpcode_Deopt[0]); i++) {
        _PyOpcode_Deopt[i] = _CiOpcode_Deopt[i];
    }
#endif
}

static void
_Ci_specialize(_Py_CODEUNIT *next_instr, int opcode)
{
    (next_instr - 1)->op.code = opcode;
}

int load_method_static_cached_oparg(Py_ssize_t slot, bool is_classmethod) {
    return (slot << 1) | (is_classmethod ? 1 : 0);
}

bool load_method_static_cached_oparg_is_classmethod(int oparg) {
    return (oparg & 1) != 0;
}

Py_ssize_t load_method_static_cached_oparg_slot(int oparg) {
    return oparg >> 1;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-label"
#elif defined(_MSC_VER) /* MS_WINDOWS */
#  pragma warning(push)
#  pragma warning(disable:4102)
#endif

#ifdef Py_DEBUG
#define ASSERT_WITHIN_STACK_BOUNDS(F, L) _Py_assert_within_stack_bounds(frame, stack_pointer, (F), (L))
#else
#define ASSERT_WITHIN_STACK_BOUNDS(F, L) (void)0
#endif


// CO_NO_MONITORING_EVENTS indicates the code object is read-only and therefore
// cannot have code-extra data added.
#define CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE                            \
  do {                                                                       \
    PyObject* executable = PyStackRef_AsPyObjectBorrow(frame->f_executable); \
    if (PyCode_Check(executable)) {                                          \
      PyCodeObject* code = (PyCodeObject*)executable;                        \
      if (!(code->co_flags & CO_NO_MONITORING_EVENTS)) {                     \
        CodeExtra* extra = codeExtra(code);                                  \
        adaptive_enabled = extra != NULL && is_adaptive_enabled(extra);      \
      }                                                                      \
    }                                                                        \
  } while (0);

#define CI_UPDATE_CALL_COUNT                                                 \
  do {                                                                       \
    PyObject* executable = PyStackRef_AsPyObjectBorrow(frame->f_executable); \
    if (PyCode_Check(executable)) {                                          \
      PyCodeObject* code = (PyCodeObject*)executable;                        \
      if (!(code->co_flags & CO_NO_MONITORING_EVENTS)) {                     \
        CodeExtra* extra = codeExtra(code);                                  \
        if (extra == NULL) {                                                 \
          adaptive_enabled = false;                                          \
        } else {                                                             \
          extra->calls += 1;                                                 \
          adaptive_enabled = is_adaptive_enabled(extra);                     \
        }                                                                    \
      }                                                                      \
    }                                                                        \
  } while (0);

  #undef DISPATCH_INLINED

  #define DISPATCH_INLINED(NEW_FRAME)                     \
    do {                                                \
        _PyFrame_SetStackPointer(frame, stack_pointer); \
        assert((NEW_FRAME)->previous == frame);         \
        frame = tstate->current_frame = (NEW_FRAME);     \
        CALL_STAT_INC(inlined_py_calls);                \
        JUMP_TO_LABEL(start_frame);                      \
    } while (0)

#undef IS_PEP523_HOOKED

#define IS_PEP523_HOOKED(tstate)         \
  (tstate->interp->eval_frame != NULL && \
   tstate->interp->eval_frame != Ci_EvalFrame)



#ifdef ENABLE_INTERPRETER_LOOP

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
#if USE_COMPUTED_GOTOS && !Py_TAIL_CALL_INTERP
/* Import the static jump table */
#include "cinderx/Interpreter/cinderx_opcode_targets.h"
void **opcode_targets = opcode_targets_table;
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
#if !Py_TAIL_CALL_INTERP
    uint8_t opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    assert(tstate->current_frame == NULL || tstate->current_frame->stackpointer != NULL);
#endif
    _PyEntryFrame entry;

    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */
    _Py_CODEUNIT *next_instr;
    _PyStackRef *stack_pointer;
    entry.stack[0] = PyStackRef_NULL;
#ifdef Py_STACKREF_DEBUG
    entry.frame.f_funcobj = PyStackRef_None;
#elif defined(Py_DEBUG)
    /* Set these to invalid but identifiable values for debugging. */
    entry.frame.f_funcobj = (_PyStackRef){.bits = 0xaaa0};
    entry.frame.f_locals = (PyObject*)0xaaa1;
    entry.frame.frame_obj = (PyFrameObject*)0xaaa2;
    entry.frame.f_globals = (PyObject*)0xaaa3;
    entry.frame.f_builtins = (PyObject*)0xaaa4;
#endif
    entry.frame.f_executable = PyStackRef_None;
    entry.frame.instr_ptr = (_Py_CODEUNIT *)_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS_PTR + 1;
    entry.frame.stackpointer = entry.stack;
    entry.frame.owner = FRAME_OWNED_BY_INTERPRETER;
    entry.frame.visited = 0;
    entry.frame.return_offset = 0;
#ifdef Py_DEBUG
    entry.frame.lltrace = 0;
#endif
    /* Push frame */
    entry.frame.previous = tstate->current_frame;
    frame->previous = &entry.frame;
    tstate->current_frame = frame;
    entry.frame.localsplus[0] = PyStackRef_NULL;
#ifdef _Py_TIER2
    if (tstate->current_executor != NULL) {
        entry.frame.localsplus[0] = PyStackRef_FromPyObjectNew(tstate->current_executor);
        tstate->current_executor = NULL;
    }
#endif

    bool adaptive_enabled = false;

    // Suppress unused variable warning because it's too hard to improve the
    // variable's scope to avoid an unused-but-set-variable warning.
    (void)adaptive_enabled; 

    /* support for generator.throw() */
    if (throwflag) {
        if (_Py_EnterRecursivePy(tstate)) {
            goto early_exit;
        }
#ifdef Py_GIL_DISABLED
        /* Load thread-local bytecode */
        if (frame->tlbc_index != ((_PyThreadStateImpl *)tstate)->tlbc_index) {
            _Py_CODEUNIT *bytecode =
                _PyEval_GetExecutableCode(tstate, _PyFrame_GetCode(frame));
            if (bytecode == NULL) {
                goto early_exit;
            }
            ptrdiff_t off = frame->instr_ptr - _PyFrame_GetBytecode(frame);
            frame->tlbc_index = ((_PyThreadStateImpl *)tstate)->tlbc_index;
            frame->instr_ptr = bytecode + off;
        }
#endif
        /* Because this avoids the RESUME, we need to update instrumentation */
        _Py_Instrument(_PyFrame_GetCode(frame), tstate->interp);
        next_instr = frame->instr_ptr;
        monitor_throw(tstate, frame, next_instr);
        stack_pointer = _PyFrame_GetStackPointer(frame);
#if Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, 0, lastopcode, adaptive_enabled);
#   else
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, 0, adaptive_enabled);
#   endif
#else
        goto error;
#endif
    }

    #if defined(_Py_TIER2) && !defined(_Py_JIT)
    /* Tier 2 interpreter state */
    _PyExecutorObject *current_executor = NULL;
    const _PyUOpInstruction *next_uop = NULL;
#endif
#if Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, 0, lastopcode, adaptive_enabled);
#   else
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, 0, adaptive_enabled);
#   endif
#else
    goto start_frame;
#include "cinderx/Interpreter/3.15/Includes/generated_cases.c.h"
#endif


#ifdef _Py_TIER2

// Tier 2 is also here!
enter_tier_two:

#ifdef _Py_JIT
    assert(0);
#else

#undef LOAD_IP
#define LOAD_IP(UNUSED) (void)0

#ifdef Py_STATS
// Disable these macros that apply to Tier 1 stats when we are in Tier 2
#undef STAT_INC
#define STAT_INC(opname, name) ((void)0)
#undef STAT_DEC
#define STAT_DEC(opname, name) ((void)0)
#endif

#undef ENABLE_SPECIALIZATION
#define ENABLE_SPECIALIZATION 0
#undef ENABLE_SPECIALIZATION_FT
#define ENABLE_SPECIALIZATION_FT 0

    ; // dummy statement after a label, before a declaration
    uint16_t uopcode;
#ifdef Py_STATS
    int lastuop = 0;
    uint64_t trace_uop_execution_counter = 0;
#endif

    assert(next_uop->opcode == _START_EXECUTOR);
tier2_dispatch:
    for (;;) {
        uopcode = next_uop->opcode;
#ifdef Py_DEBUG
        if (frame->lltrace >= 3) {
            dump_stack(frame, stack_pointer);
            if (next_uop->opcode == _START_EXECUTOR) {
                printf("%4d uop: ", 0);
            }
            else {
                printf("%4d uop: ", (int)(next_uop - current_executor->trace));
            }
            _PyUOpPrint(next_uop);
            printf("\n");
        }
#endif
        next_uop++;
        OPT_STAT_INC(uops_executed);
        UOP_STAT_INC(uopcode, execution_count);
        UOP_PAIR_INC(uopcode, lastuop);
#ifdef Py_STATS
        trace_uop_execution_counter++;
        ((_PyUOpInstruction  *)next_uop)[-1].execution_count++;
#endif

        switch (uopcode) {

#include "executor_cases.c.h"

            default:
#ifdef Py_DEBUG
            {
                printf("Unknown uop: ");
                _PyUOpPrint(&next_uop[-1]);
                printf(" @ %d\n", (int)(next_uop - current_executor->trace - 1));
                Py_FatalError("Unknown uop");
            }
#else
            Py_UNREACHABLE();
#endif

        }
    }

jump_to_error_target:
#ifdef Py_DEBUG
    if (frame->lltrace >= 2) {
        printf("Error: [UOp ");
        _PyUOpPrint(&next_uop[-1]);
        printf(" @ %d -> %s]\n",
               (int)(next_uop - current_executor->trace - 1),
               _PyOpcode_OpName[frame->instr_ptr->op.code]);
    }
#endif
    assert(next_uop[-1].format == UOP_FORMAT_JUMP);
    uint16_t target = uop_get_error_target(&next_uop[-1]);
    next_uop = current_executor->trace + target;
    goto tier2_dispatch;

jump_to_jump_target:
    assert(next_uop[-1].format == UOP_FORMAT_JUMP);
    target = uop_get_jump_target(&next_uop[-1]);
    next_uop = current_executor->trace + target;
    goto tier2_dispatch;

#endif  // _Py_JIT

#endif // _Py_TIER2

early_exit:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallPy(tstate);
    assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
    // GH-99729: We need to unlink the frame *before* clearing it:
    _PyInterpreterFrame *dying = frame;
    frame = tstate->current_frame = dying->previous;
    _PyEval_FrameClearAndPop(tstate, dying);
    frame->return_offset = 0;
    assert(frame->owner == FRAME_OWNED_BY_INTERPRETER);
    /* Restore previous frame and exit */
    tstate->current_frame = frame->previous;
    return NULL;
}

#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER) /* MS_WINDOWS */
#  pragma warning(pop)
#endif

// clang-format on
static int
_Ci_CheckArgs(PyThreadState* tstate, _PyInterpreterFrame* f, PyCodeObject* co) {
  // In the future we can use co_extra to store the cached arg info
  _PyStackRef* fastlocals = &f->localsplus[0];

  PyObject* checks = _PyClassLoader_GetCodeArgumentTypeDescrs(co);
  PyObject* local;
  PyObject* type_descr;
  PyTypeObject* type;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    local = PyTuple_GET_ITEM(checks, i);
    type_descr = PyTuple_GET_ITEM(checks, i + 1);
    long idx = PyLong_AsLong(local);
    assert(idx >= 0);
    PyObject* val = PyStackRef_AsPyObjectBorrow(fastlocals[idx]);

    int optional;
    int exact;
    type = _PyClassLoader_ResolveType(type_descr, &optional, &exact);
    if (type == NULL) {
      return -1;
    }

    int primitive = _PyClassLoader_GetTypeCode(type);
    if (primitive == TYPED_BOOL) {
      optional = 0;
      Py_DECREF(type);
      type = &PyBool_Type;
      Py_INCREF(type);
    } else if (primitive <= TYPED_INT64) {
      exact = optional = 0;
      Py_DECREF(type);
      type = &PyLong_Type;
      Py_INCREF(type);
    } else if (primitive == TYPED_DOUBLE) {
      exact = optional = 0;
      Py_DECREF(type);
      type = &PyFloat_Type;
      Py_INCREF(type);
    } else {
      assert(primitive == TYPED_OBJECT);
    }

    if (!_PyObject_TypeCheckOptional(val, type, optional, exact)) {
      PyErr_Format(
          CiExc_StaticTypeError,
          "%U expected '%s' for argument %U, got '%s'",
          co->co_name,
          type->tp_name,
          PyTuple_GET_ITEM(co->co_localsplusnames, idx),
          Py_TYPE(val)->tp_name);
      Py_DECREF(type);
      return -1;
    }

    Py_DECREF(type);

    if (primitive <= TYPED_INT64) {
      size_t value;
      if (!_PyClassLoader_OverflowCheck(val, primitive, &value)) {
        PyErr_SetString(PyExc_OverflowError, "int overflow");
        return -1;
      }
    }
  }
  return 0;
}

static PyObject* _CiStaticEval_Vector(
    PyThreadState* tstate,
    PyFunctionObject* func,
    PyObject* locals,
    PyObject* const* args,
    size_t argcount,
    PyObject* kwnames,
    int check_args) {
    size_t total_args = argcount;
    if (kwnames) {
        total_args += PyTuple_GET_SIZE(kwnames);
    }
    _PyStackRef stack_array[8];
    _PyStackRef *arguments;
    if (total_args <= 8) {
        arguments = stack_array;
    }
    else {
        arguments = PyMem_Malloc(sizeof(_PyStackRef) * total_args);
        if (arguments == NULL) {
            return PyErr_NoMemory();
        }
    }
    /* _PyEvalFramePushAndInit consumes the references
        * to func, locals and all its arguments */
    Py_XINCREF(locals);
    for (size_t i = 0; i < argcount; i++) {
        arguments[i] = PyStackRef_FromPyObjectNew(args[i]);
    }
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            arguments[i+argcount] = PyStackRef_FromPyObjectNew(args[i+argcount]);
        }
    }
    _PyInterpreterFrame *frame = _PyEvalFramePushAndInit(
        tstate, PyStackRef_FromPyObjectNew(func), locals,
        arguments, argcount, kwnames, NULL);
    if (total_args > 8) {
        PyMem_Free(arguments);
    }
    if (frame == NULL) {
        return NULL;
    }

    EVAL_CALL_STAT_INC(EVAL_CALL_VECTOR);
#ifdef ENABLE_INTERPRETER_LOOP
    PyCodeObject* co = (PyCodeObject*)func->func_code;
    assert(co->co_flags & CI_CO_STATICALLY_COMPILED);
    if (check_args && _Ci_CheckArgs(tstate, frame, co) < 0) {
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }

    return Ci_EvalFrame(tstate, frame, 0);
#else
    return _PyEval_EvalFrameDefault(tstate, frame, 0);
#endif
}

PyObject* Ci_StaticFunction_Vectorcall(
    PyObject* func,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  assert(PyFunction_Check(func));
  PyFunctionObject* f = (PyFunctionObject*)func;
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  assert(nargs >= 0);
  assert(nargs == 0 || stack != NULL);

  PyCodeObject* code = (PyCodeObject*)f->func_code;
  PyObject* globals = (code->co_flags & CO_OPTIMIZED) ? NULL : f->func_globals;

  PyThreadState* tstate = _PyThreadState_GET();
  return _CiStaticEval_Vector(tstate, f, globals, stack, nargs, kwnames, 1);
}

PyObject* _Py_HOT_FUNCTION Ci_PyFunction_CallStatic(
    PyFunctionObject* func,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  assert(PyFunction_Check(func));
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  assert(nargs == 0 || args != NULL);

  /* We are bound to a specific function that is known at compile time, and
   * all of the arguments are guaranteed to be provided */
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  assert(co->co_argcount == nargs);
  assert(co->co_flags & CI_CO_STATICALLY_COMPILED);
  assert(co->co_flags & CO_OPTIMIZED);
  assert(kwnames == NULL);

  /* Silence unused variable warnings. */
  (void)co;
  (void)kwnames;
  (void)nargs;

  PyThreadState* tstate = _PyThreadState_GET();
  assert(tstate != NULL);

  return _CiStaticEval_Vector(tstate, func, NULL, args, nargsf, NULL, 0);
}
