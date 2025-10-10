// Copyright (c) Meta Platforms, Inc. and affiliates.

// clang-format off

#define CINDERX_INTERPRETER

#include "cinderx/Common/code.h"

// Must come after pycore_opcode, we want to get the exported ones.
#define NEED_OPCODE_NAMES
#define NEED_OPCODE_TABLES
#include "cinderx/Interpreter/cinder_opcode.h"

#include "cinderx/Interpreter/3.12/Includes/ceval.c"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/static_array.h"

#include "cinderx/Jit/generators_rt.h"

/* _PyEval_EvalFrameDefault() is a *big* function,
 * so consume 3 units of C stack */
#define PY_EVAL_C_STACK_UNITS 2

// These are used to truncate primitives/check signed bits when converting
// between them
static uint64_t trunc_masks[] = {0xFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF};
static uint64_t signed_bits[] = {0x80, 0x8000, 0x80000000, 0x8000000000000000};
static uint64_t signex_masks[] = {
    0xFFFFFFFFFFFFFF00,
    0xFFFFFFFFFFFF0000,
    0xFFFFFFFF00000000,
    0x0};

static int8_t unbox_primitive_bool_and_decref(PyObject* x) {
    assert(PyBool_Check(x));
    int8_t res = (x == Py_True) ? 1 : 0;
    Py_DECREF(x);
    return res;
}

static Py_ssize_t unbox_primitive_int_and_decref(PyObject* x) {
    assert(PyLong_Check(x));
    Py_ssize_t res = (Py_ssize_t)PyLong_AsVoidPtr(x);
    Py_DECREF(x);
    return res;
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

static PyObject* sign_extend_primitive(PyObject *obj, int type) {
    if ((type & (TYPED_INT_SIGNED)) && type != (TYPED_DOUBLE)) {
        /* We have a boxed value on the stack already, but we may have to
        * deal with sign extension */
        PyObject* val = obj;
        size_t ival = (size_t)PyLong_AsVoidPtr(val);
        if (ival & ((size_t)1) << 63) {
            obj = PyLong_FromSsize_t((int64_t)ival);
            Py_DECREF(val);
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
            *(int8_t*)addr = (int8_t)unbox_primitive_bool_and_decref(value);
            break;
        case TYPED_INT8:
            *(int8_t*)addr = (int8_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_INT16:
            *(int16_t*)addr = (int16_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_INT32:
            *(int32_t*)addr = (int32_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_INT64:
            *(int64_t*)addr = (int64_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_UINT8:
            *(uint8_t*)addr = (uint8_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_UINT16:
            *(uint16_t*)addr = (uint16_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_UINT32:
            *(uint32_t*)addr = (uint32_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_UINT64:
            *(uint64_t*)addr = (uint64_t)unbox_primitive_int_and_decref(value);
            break;
        case TYPED_DOUBLE:
            *((double*)addr) = PyFloat_AsDouble(value);
            Py_DECREF(value);
            break;
        default:
            PyErr_SetString(PyExc_RuntimeError, "unsupported field type");
            break;
    }
}

#define FIELD_OFFSET(self, offset) (PyObject**)(((char*)self) + offset)

#define CAST_COERCE_OR_ERROR(val, type, exact)                                     \
    if (type == &PyFloat_Type && PyObject_TypeCheck(val, &PyLong_Type)) {          \
        long lval = PyLong_AsLong(val);                                            \
        Py_DECREF(val);                                                            \
        val = PyFloat_FromDouble(lval);                                            \
    } else {                                                                       \
        PyErr_Format(                                                              \
            PyExc_TypeError,                                                       \
            exact ? "expected exactly '%s', got '%s'" : "expected '%s', got '%s'", \
            type->tp_name,                                                         \
            Py_TYPE(val)->tp_name);                                                \
        Py_DECREF(type);                                                           \
        goto error;                                                                \
    }

static Py_ssize_t invoke_function_args(PyObject *consts, int oparg)
{
    PyObject* value = GETITEM(consts, oparg);
    Py_ssize_t nargs = PyLong_AsLong(PyTuple_GET_ITEM(value, 1));
    return nargs;
}

static Py_ssize_t invoke_native_args(PyObject *consts, int oparg)
{
    PyObject* value = GETITEM(consts, oparg);
    PyObject* signature = PyTuple_GET_ITEM(value, 1);
    return PyTuple_GET_SIZE(signature) - 1;
}

static Py_ssize_t build_checked_obj_size(PyObject *consts, int oparg)
{
    PyObject* map_info = GETITEM(consts, oparg);
    return PyLong_AsLong(PyTuple_GET_ITEM(map_info, 1));
}

static int ci_build_dict(PyObject **map_items, Py_ssize_t map_size, PyObject *map)
{
    for (Py_ssize_t i = 0; i < map_size; i++) {
        PyObject* key = map_items[2 * i];
        PyObject* value = map_items[2 * i + 1];
        if (Ci_CheckedDict_SetItem(map, key, value) < 0) {
            return -1;
        }
    }
    return 0;
}

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

#define _PyOpcode_Caches _CiOpcode_Caches

bool Ci_DelayAdaptiveCode = false;
uint64_t Ci_AdaptiveThreshold = 80;

bool is_adaptive_enabled(CodeExtra *extra) {
    return !Ci_DelayAdaptiveCode || extra->calls > Ci_AdaptiveThreshold;
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

#define CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE \
    PyCodeObject* code = frame->f_code; \
    CodeExtra *extra = codeExtra(code); \
    adaptive_enabled = extra != NULL && is_adaptive_enabled(extra);

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    _Py_EnsureTstateNotNULL(tstate);
    CALL_STAT_INC(pyeval_calls);

#if USE_COMPUTED_GOTOS
/* Import the static jump table */
#include "cinderx/Interpreter/cinderx_opcode_targets.h"
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
    // opcode is an 8-bit value to improve the code generated by MSVC
    // for the big switch below (in combination with the EXTRA_CASES macro).
    uint8_t opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
#ifdef LLTRACE
    int lltrace = 0;
#endif

    _PyCFrame cframe;
    _PyInterpreterFrame  entry_frame;
    PyObject *kwnames = NULL; // Borrowed reference. Reset by CALL instructions.

    /* WARNING: Because the _PyCFrame lives on the C stack,
     * but can be accessed from a heap allocated object (tstate)
     * strict stack discipline must be maintained.
     */
    _PyCFrame *prev_cframe = tstate->cframe;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;

    assert(tstate->interp->interpreter_trampoline != NULL);
#ifdef Py_DEBUG
    /* Set these to invalid but identifiable values for debugging. */
    entry_frame.f_funcobj = (PyObject*)0xaaa0;
    entry_frame.f_locals = (PyObject*)0xaaa1;
    entry_frame.frame_obj = (PyFrameObject*)0xaaa2;
    entry_frame.f_globals = (PyObject*)0xaaa3;
    entry_frame.f_builtins = (PyObject*)0xaaa4;
#endif
    entry_frame.f_code = tstate->interp->interpreter_trampoline;
    entry_frame.prev_instr =
        _PyCode_CODE(tstate->interp->interpreter_trampoline);
    entry_frame.stacktop = 0;
    entry_frame.owner = FRAME_OWNED_BY_CSTACK;
    entry_frame.return_offset = 0;
    /* Push frame */
    entry_frame.previous = prev_cframe->current_frame;
    frame->previous = &entry_frame;
    cframe.current_frame = frame;

    tstate->c_recursion_remaining -= (PY_EVAL_C_STACK_UNITS - 1);
    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        tstate->c_recursion_remaining--;
        tstate->py_recursion_remaining--;
        goto exit_unwind;
    }

    /* support for generator.throw() */
    bool adaptive_enabled = false;
    if (throwflag) {
        if (_Py_EnterRecursivePy(tstate)) {
            goto exit_unwind;
        }
        /* Because this avoids the RESUME,
         * we need to update instrumentation */
        _Py_Instrument(frame->f_code, tstate->interp);
        monitor_throw(tstate, frame, frame->prev_instr);
        /* TO DO -- Monitor throw entry. */
        goto resume_with_error;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */

    _Py_CODEUNIT *next_instr;
    PyObject **stack_pointer;

/* Sets the above local variables from the frame */
#define SET_LOCALS_FROM_FRAME() \
    assert(_PyInterpreterFrame_LASTI(frame) >= -1); \
    /* Jump back to the last instruction executed... */ \
    next_instr = frame->prev_instr + 1; \
    stack_pointer = _PyFrame_GetStackPointer(frame);

start_frame:
    // Update call count.
    {
        PyCodeObject* code = frame->f_code;
        CodeExtra *extra = codeExtra(code);
        if (extra != NULL) {
            extra->calls += 1;
            adaptive_enabled = is_adaptive_enabled(extra);
        } else {
            adaptive_enabled = false;
        }
    }

    if (_Py_EnterRecursivePy(tstate)) {
        goto exit_unwind;
    }

resume_frame:
    SET_LOCALS_FROM_FRAME();

#ifdef LLTRACE
    {
        if (frame != &entry_frame) {
            int r = PyDict_Contains(GLOBALS(), &_Py_ID(__lltrace__));
            if (r < 0) {
                goto exit_unwind;
            }
            lltrace = r;
        }
        if (lltrace) {
            lltrace_resume_frame(frame);
        }
    }
#endif

#ifdef Py_DEBUG
    /* _PyEval_EvalFrameDefault() must not be called with an exception set,
       because it can clear it (directly or indirectly) and so the
       caller loses its exception */
    assert(!_PyErr_Occurred(tstate));
#endif

    DISPATCH();

handle_eval_breaker:

    /* Do periodic things, like check for signals and async I/0.
     * We need to do reasonably frequently, but not too frequently.
     * All loops should include a check of the eval breaker.
     * We also check on return from any builtin function.
     *
     * ## More Details ###
     *
     * The eval loop (this function) normally executes the instructions
     * of a code object sequentially.  However, the runtime supports a
     * number of out-of-band execution scenarios that may pause that
     * sequential execution long enough to do that out-of-band work
     * in the current thread using the current PyThreadState.
     *
     * The scenarios include:
     *
     *  - cyclic garbage collection
     *  - GIL drop requests
     *  - "async" exceptions
     *  - "pending calls"  (some only in the main thread)
     *  - signal handling (only in the main thread)
     *
     * When the need for one of the above is detected, the eval loop
     * pauses long enough to handle the detected case.  Then, if doing
     * so didn't trigger an exception, the eval loop resumes executing
     * the sequential instructions.
     *
     * To make this work, the eval loop periodically checks if any
     * of the above needs to happen.  The individual checks can be
     * expensive if computed each time, so a while back we switched
     * to using pre-computed, per-interpreter variables for the checks,
     * and later consolidated that to a single "eval breaker" variable
     * (now a PyInterpreterState field).
     *
     * For the longest time, the eval breaker check would happen
     * frequently, every 5 or so times through the loop, regardless
     * of what instruction ran last or what would run next.  Then, in
     * early 2021 (gh-18334, commit 4958f5d), we switched to checking
     * the eval breaker less frequently, by hard-coding the check to
     * specific places in the eval loop (e.g. certain instructions).
     * The intent then was to check after returning from calls
     * and on the back edges of loops.
     *
     * In addition to being more efficient, that approach keeps
     * the eval loop from running arbitrary code between instructions
     * that don't handle that well.  (See gh-74174.)
     *
     * Currently, the eval breaker check happens here at the
     * "handle_eval_breaker" label.  Some instructions come here
     * explicitly (goto) and some indirectly.  Notably, the check
     * happens on back edges in the control flow graph, which
     * pretty much applies to all loops and most calls.
     * (See bytecodes.c for exact information.)
     *
     * One consequence of this approach is that it might not be obvious
     * how to force any specific thread to pick up the eval breaker,
     * or for any specific thread to not pick it up.  Mostly this
     * involves judicious uses of locks and careful ordering of code,
     * while avoiding code that might trigger the eval breaker
     * until so desired.
     */
    if (_Py_HandlePending(tstate) != 0) {
        goto error;
    }
    DISPATCH();

    {
    /* Start instructions */
#if !USE_COMPUTED_GOTOS
    dispatch_opcode:
        switch (opcode)
#endif
        {

#include "cinderx/Interpreter/3.12/Includes/generated_cases.c.h"

    /* INSTRUMENTED_LINE has to be here, rather than in bytecodes.c,
     * because it needs to capture frame->prev_instr before it is updated,
     * as happens in the standard instruction prologue.
     */
#if USE_COMPUTED_GOTOS
        TARGET_INSTRUMENTED_LINE:
#else
        case INSTRUMENTED_LINE:
#endif
    {
        _Py_CODEUNIT *prev = frame->prev_instr;
        _Py_CODEUNIT *here = frame->prev_instr = next_instr;
        _PyFrame_SetStackPointer(frame, stack_pointer);
        int original_opcode = _Py_call_instrumentation_line(
                tstate, frame, here, prev);
        stack_pointer = _PyFrame_GetStackPointer(frame);
        if (original_opcode < 0) {
            next_instr = here+1;
            goto error;
        }
        next_instr = frame->prev_instr;
        if (next_instr != here) {
            DISPATCH();
        }
        if (_CiOpcode_Caches[original_opcode]) {
            _PyBinaryOpCache *cache = (_PyBinaryOpCache *)(next_instr+1);
            /* Prevent the underlying instruction from specializing
             * and overwriting the instrumentation. */
            INCREMENT_ADAPTIVE_COUNTER(cache->counter);
        }
        opcode = original_opcode;
        DISPATCH_GOTO();
    }


#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#else
        EXTRA_CASES  // From cinder_opcode.h, a 'case' for each unused opcode
#endif
            /* Tell C compilers not to hold the opcode variable in the loop.
               next_instr points the current instruction without TARGET(). */
            opcode = next_instr->op.code;
            _PyErr_Format(tstate, PyExc_SystemError,
                          "%U:%d: unknown opcode %d",
                          frame->f_code->co_filename,
                          PyUnstable_InterpreterFrame_GetLine(frame),
                          opcode);
            goto error;

        } /* End instructions */

        /* This should never be reached. Every opcode should end with DISPATCH()
           or goto error. */
        Py_UNREACHABLE();

unbound_local_error:
        {
            format_exc_check_arg(tstate, PyExc_UnboundLocalError,
                UNBOUNDLOCAL_ERROR_MSG,
                PyTuple_GetItem(frame->f_code->co_localsplusnames, oparg)
            );
            goto error;
        }

pop_4_error:
    STACK_SHRINK(1);
pop_3_error:
    STACK_SHRINK(1);
pop_2_error:
    STACK_SHRINK(1);
pop_1_error:
    STACK_SHRINK(1);
error:
        kwnames = NULL;
        /* Double-check exception status. */
#ifdef NDEBUG
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_SystemError,
                             "error return without exception set");
        }
#else
        assert(_PyErr_Occurred(tstate));
#endif

        /* Log traceback info. */
        assert(frame != &entry_frame);
        if (!_PyFrame_IsIncomplete(frame)) {
            PyFrameObject *f = _PyFrame_GetFrameObject(frame);
            if (f != NULL) {
                PyTraceBack_Here(f);
            }
        }
        monitor_raise(tstate, frame, next_instr-1);
exception_unwind:
        {
            /* We can't use frame->f_lasti here, as RERAISE may have set it */
            int offset = INSTR_OFFSET()-1;
            int level, handler, lasti;
            if (get_exception_handler(frame->f_code, offset, &level, &handler, &lasti) == 0) {
                // No handlers, so exit.
                assert(_PyErr_Occurred(tstate));

                /* Pop remaining stack entries. */
                PyObject **stackbase = _PyFrame_Stackbase(frame);
                while (stack_pointer > stackbase) {
                    PyObject *o = POP();
                    Py_XDECREF(o);
                }
                assert(STACK_LEVEL() == 0);
                _PyFrame_SetStackPointer(frame, stack_pointer);
                monitor_unwind(tstate, frame, next_instr-1);
                goto exit_unwind;
            }

            assert(STACK_LEVEL() >= level);
            PyObject **new_top = _PyFrame_Stackbase(frame) + level;
            while (stack_pointer > new_top) {
                PyObject *v = POP();
                Py_XDECREF(v);
            }
            if (lasti) {
                int frame_lasti = _PyInterpreterFrame_LASTI(frame);
                PyObject *lasti_obj = PyLong_FromLong(frame_lasti);
                if (lasti_obj == NULL) {
                    goto exception_unwind;
                }
                PUSH(lasti_obj);
            }

            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            PyObject *exc = _PyErr_GetRaisedException(tstate);
            PUSH(exc);
            JUMPTO(handler);
            if (monitor_handled(tstate, frame, next_instr, exc) < 0) {
                goto exception_unwind;
            }
            /* Resume normal execution */
            DISPATCH();
        }
    }

exit_unwind:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallPy(tstate);
    assert(frame != &entry_frame);
    // GH-99729: We need to unlink the frame *before* clearing it:
    _PyInterpreterFrame *dying = frame;
    frame = cframe.current_frame = dying->previous;
    _PyEvalFrameClearAndPop(tstate, dying);
    frame->return_offset = 0;
    if (frame == &entry_frame) {
        /* Restore previous cframe and exit */
        tstate->cframe = cframe.previous;
        assert(tstate->cframe->current_frame == frame->previous);
        tstate->c_recursion_remaining += PY_EVAL_C_STACK_UNITS;
        return NULL;
    }

resume_with_error:
    SET_LOCALS_FROM_FRAME();
    goto error;

}

// clang-format on
static int
_Ci_CheckArgs(PyThreadState* tstate, _PyInterpreterFrame* f, PyCodeObject* co) {
  // In the future we can use co_extra to store the cached arg info
  PyObject** fastlocals = &f->localsplus[0];

  PyObject* checks = _PyClassLoader_GetCodeArgumentTypeDescrs(co);
  PyObject* local;
  PyObject* type_descr;
  PyTypeObject* type;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    local = PyTuple_GET_ITEM(checks, i);
    type_descr = PyTuple_GET_ITEM(checks, i + 1);
    long idx = PyLong_AsLong(local);
    assert(idx >= 0);
    PyObject* val = fastlocals[idx];

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
  Py_INCREF(func);
  Py_XINCREF(locals);
  for (size_t i = 0; i < argcount; i++) {
    Py_INCREF(args[i]);
  }
  if (kwnames) {
    Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t i = 0; i < kwcount; i++) {
      Py_INCREF(args[i + argcount]);
    }
  }
  _PyInterpreterFrame* frame =
      _PyEvalFramePushAndInit(tstate, func, locals, args, argcount, kwnames);
  if (frame == NULL) {
    return NULL;
  }

  PyCodeObject* co = (PyCodeObject*)func->func_code;
  assert(co->co_flags & CI_CO_STATICALLY_COMPILED);
  if (check_args && _Ci_CheckArgs(tstate, frame, co) < 0) {
    _PyEvalFrameClearAndPop(tstate, frame);
    return NULL;
  }

  EVAL_CALL_STAT_INC(EVAL_CALL_VECTOR);
  return Ci_EvalFrame(tstate, frame, 0);
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
