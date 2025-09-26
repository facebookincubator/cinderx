// Copyright (c) Meta Platforms, Inc. and affiliates.

// This file contains instruction definitions.
// It is read by generators stored in Tools/cases_generator/
// to generate Python/generated_cases.c.h and others.
// Note that there is some dummy C code at the top and bottom of the file
// to fool text editors like VS Code into believing this is valid C code.
// The actual instruction definitions start at // BEGIN BYTECODES //.
// See Tools/cases_generator/README.md for more information.

#include "Python.h"
#include "pycore_abstract.h"      // _PyIndex_Check()
#include "pycore_audit.h"         // _PySys_Audit()
#include "pycore_backoff.h"
#include "pycore_cell.h"          // PyCell_GetRef()
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_emscripten_signal.h"  // _Py_CHECK_EMSCRIPTEN_SIGNALS
#include "pycore_function.h"
#include "pycore_instruments.h"
#include "pycore_interpolation.h" // _PyInterpolation_Build()
#include "pycore_intrinsics.h"
#include "pycore_long.h"          // _PyLong_ExactDealloc(), _PyLong_GetZero()
#include "pycore_moduleobject.h"  // PyModuleObject
#include "pycore_object.h"        // _PyObject_GC_TRACK()
#include "pycore_opcode_metadata.h"  // uop names
#include "pycore_opcode_utils.h"  // MAKE_FUNCTION_*
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_*
#include "pycore_pyerrors.h"      // _PyErr_GetRaisedException()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_range.h"         // _PyRangeIterObject
#include "pycore_setobject.h"     // _PySet_NextEntry()
#include "pycore_sliceobject.h"   // _PyBuildSlice_ConsumeRefs
#include "pycore_stackref.h"
#include "pycore_template.h"      // _PyTemplate_Build()
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_typeobject.h"    // _PySuper_Lookup()

#include "pycore_dict.h"
#include "dictobject.h"
#include "pycore_frame.h"
#include "opcode.h"
#include "optimizer.h"
#include "pydtrace.h"
#include "setobject.h"


#define USE_COMPUTED_GOTOS 0
#include "ceval_macros.h"

/* Flow control macros */

#define inst(name, ...) case name:
#define op(name, ...) /* NAME is ignored */
#define macro(name) static int MACRO_##name
#define super(name) static int SUPER_##name
#define family(name, ...) static int family_##name
#define pseudo(name) static int pseudo_##name
#define label(name) name:

/* Annotations */
#define guard
#define override
#define specializing
#define replicate(TIMES)
#define tier1
#define no_save_ip

// Dummy variables for stack effects.
static PyObject *value, *value1, *value2, *left, *right, *res, *sum, *prod, *sub;
static PyObject *container, *start, *stop, *v, *lhs, *rhs, *res2;
static PyObject *list, *tuple, *dict, *owner, *set, *str, *tup, *map, *keys;
static PyObject *exit_func, *lasti, *val, *retval, *obj, *iter, *exhausted;
static PyObject *aiter, *awaitable, *iterable, *w, *exc_value, *bc, *locals;
static PyObject *orig, *excs, *update, *b, *fromlist, *level, *from;
static PyObject **pieces, **values;
static size_t jump;
// Dummy variables for cache effects
static uint16_t invert, counter, index, hint;
#define unused 0  // Used in a macro def, can't be static
static uint32_t type_version;
static _PyExecutorObject *current_executor;

static PyObject *
dummy_func(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame,
    unsigned char opcode,
    unsigned int oparg,
    _Py_CODEUNIT *next_instr,
    PyObject **stack_pointer,
    int throwflag,
    PyObject *args[]
)
{
    // Dummy labels.
    pop_1_error:
    // Dummy locals.
    PyObject *dummy;
    _Py_CODEUNIT *this_instr;
    PyObject *attr;
    PyObject *attrs;
    PyObject *bottom;
    PyObject *callable;
    PyObject *callargs;
    PyObject *codeobj;
    PyObject *cond;
    PyObject *descr;
    PyObject *exc;
    PyObject *exit;
    PyObject *fget;
    PyObject *fmt_spec;
    PyObject *func;
    uint32_t func_version;
    PyObject *getattribute;
    PyObject *kwargs;
    PyObject *kwdefaults;
    PyObject *len_o;
    PyObject *match;
    PyObject *match_type;
    PyObject *method;
    PyObject *mgr;
    Py_ssize_t min_args;
    PyObject *names;
    PyObject *new_exc;
    PyObject *next;
    PyObject *none;
    PyObject *null;
    PyObject *prev_exc;
    PyObject *receiver;
    PyObject *rest;
    int result;
    PyObject *self;
    PyObject *seq;
    PyObject *slice;
    PyObject *step;
    PyObject *subject;
    PyObject *top;
    PyObject *type;
    PyObject *typevars;
    PyObject *val0;
    PyObject *val1;
    int values_or_none;

    switch (opcode) {

// BEGIN BYTECODES //
        override inst(LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN, (unused/1, type_version/2, func_version/2, getattribute/4, owner -- unused)) {
            PyObject *owner_o = PyStackRef_AsPyObjectBorrow(owner);

            assert((oparg & 1) == 0);
            DEOPT_IF(IS_PEP523_HOOKED(tstate));
            PyTypeObject *cls = Py_TYPE(owner_o);
            assert(type_version != 0);
            DEOPT_IF(FT_ATOMIC_LOAD_UINT_RELAXED(cls->tp_version_tag) != type_version);
            assert(Py_IS_TYPE(getattribute, &PyFunction_Type));
            PyFunctionObject *f = (PyFunctionObject *)getattribute;
            assert(func_version != 0);
            DEOPT_IF(f->func_version != func_version);
            PyCodeObject *code = (PyCodeObject *)f->func_code;
            assert(code->co_argcount == 2);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize));
            STAT_INC(LOAD_ATTR, hit);

            PyObject *name = GETITEM(FRAME_CO_NAMES, oparg >> 1);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(
                tstate, PyStackRef_FromPyObjectNew(f), 2, frame);
            new_frame->localsplus[0] = owner;
            DEAD(owner);
            // Manipulate stack directly because we exit with DISPATCH_INLINED().
            SYNC_SP();
            new_frame->localsplus[1] = PyStackRef_FromPyObjectNew(name);
            frame->return_offset = INSTRUCTION_SIZE;
            DISPATCH_INLINED(new_frame);
        }

        override op(_DO_CALL, (callable, self_or_null, args[oparg] -- res)) {
            PyObject *callable_o = PyStackRef_AsPyObjectBorrow(callable);

            // oparg counts all of the args, but *not* self:
            int total_args = oparg;
            _PyStackRef *arguments = args;
            if (!PyStackRef_IsNull(self_or_null)) {
                arguments--;
                total_args++;
            }
            // Check if the call can be inlined or not
            if (Py_TYPE(callable_o) == &PyFunction_Type &&
                !IS_PEP523_HOOKED(tstate) &&
                ((PyFunctionObject *)callable_o)->vectorcall == _PyFunction_Vectorcall)
            {
                int code_flags = ((PyCodeObject*)PyFunction_GET_CODE(callable_o))->co_flags;
                PyObject *locals = code_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(callable_o));
                _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit(
                    tstate, callable, locals,
                    arguments, total_args, NULL, frame
                );
                DEAD(args);
                DEAD(self_or_null);
                DEAD(callable);
                // Manipulate stack directly since we leave using DISPATCH_INLINED().
                SYNC_SP();
                // The frame has stolen all the arguments from the stack,
                // so there is no need to clean them up.
                if (new_frame == NULL) {
                    ERROR_NO_POP();
                }
                frame->return_offset = INSTRUCTION_SIZE;
                DISPATCH_INLINED(new_frame);
            }
            /* Callable is not a normal Python function */
            STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
            if (CONVERSION_FAILED(args_o)) {
                DECREF_INPUTS();
                ERROR_IF(true);
            }
            PyObject *res_o = PyObject_Vectorcall(
                callable_o, args_o,
                total_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
                NULL);
            STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
            if (opcode == INSTRUMENTED_CALL) {
                PyObject *arg = total_args == 0 ?
                    &_PyInstrumentation_MISSING : PyStackRef_AsPyObjectBorrow(arguments[0]);
                if (res_o == NULL) {
                    _Py_call_instrumentation_exc2(
                        tstate, PY_MONITORING_EVENT_C_RAISE,
                        frame, this_instr, callable_o, arg);
                }
                else {
                    int err = _Py_call_instrumentation_2args(
                        tstate, PY_MONITORING_EVENT_C_RETURN,
                        frame, this_instr, callable_o, arg);
                    if (err < 0) {
                        Py_CLEAR(res_o);
                    }
                }
            }
            assert((res_o != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            DECREF_INPUTS();
            ERROR_IF(res_o == NULL);
            res = PyStackRef_FromPyObjectSteal(res_o);
        }

        override op(_CHECK_PEP_523, (--)) {
            DEOPT_IF(IS_PEP523_HOOKED(tstate));
        }

        override op(_PUSH_FRAME, (new_frame: _PyInterpreterFrame* -- )) {
            // Write it out explicitly because it's subtly different.
            // Eventually this should be the only occurrence of this code.
            assert(!IS_PEP523_HOOKED(tstate));
            _PyInterpreterFrame *temp = new_frame;
            DEAD(new_frame);
            SYNC_SP();
            _PyFrame_SetStackPointer(frame, stack_pointer);
            assert(new_frame->previous == frame || new_frame->previous->previous == frame);
            CALL_STAT_INC(inlined_py_calls);
            frame = tstate->current_frame = temp;
            tstate->py_recursion_remaining--;
            LOAD_SP();
            LOAD_IP(0);
            LLTRACE_RESUME_FRAME();
        }

        override op(_DO_CALL_KW, (callable, self_or_null, args[oparg], kwnames -- res)) {
            PyObject *callable_o = PyStackRef_AsPyObjectBorrow(callable);
            PyObject *kwnames_o = PyStackRef_AsPyObjectBorrow(kwnames);

            // oparg counts all of the args, but *not* self:
            int total_args = oparg;
            _PyStackRef *arguments = args;
            if (!PyStackRef_IsNull(self_or_null)) {
                arguments--;
                total_args++;
            }
            int positional_args = total_args - (int)PyTuple_GET_SIZE(kwnames_o);
            // Check if the call can be inlined or not
            if (Py_TYPE(callable_o) == &PyFunction_Type &&
                !IS_PEP523_HOOKED(tstate) &&
                ((PyFunctionObject *)callable_o)->vectorcall == _PyFunction_Vectorcall)
            {
                int code_flags = ((PyCodeObject*)PyFunction_GET_CODE(callable_o))->co_flags;
                PyObject *locals = code_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(callable_o));
                _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit(
                    tstate, callable, locals,
                    arguments, positional_args, kwnames_o, frame
                );
                DEAD(args);
                DEAD(self_or_null);
                DEAD(callable);
                PyStackRef_CLOSE(kwnames);
                // Sync stack explicitly since we leave using DISPATCH_INLINED().
                SYNC_SP();
                // The frame has stolen all the arguments from the stack,
                // so there is no need to clean them up.
                if (new_frame == NULL) {
                    ERROR_NO_POP();
                }
                assert(INSTRUCTION_SIZE == 1 + INLINE_CACHE_ENTRIES_CALL_KW);
                frame->return_offset = INSTRUCTION_SIZE;
                DISPATCH_INLINED(new_frame);
            }
            /* Callable is not a normal Python function */
            STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
            if (CONVERSION_FAILED(args_o)) {
                DECREF_INPUTS();
                ERROR_IF(true);
            }
            PyObject *res_o = PyObject_Vectorcall(
                callable_o, args_o,
                positional_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
                kwnames_o);
            STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
            if (opcode == INSTRUMENTED_CALL_KW) {
                PyObject *arg = total_args == 0 ?
                    &_PyInstrumentation_MISSING : PyStackRef_AsPyObjectBorrow(arguments[0]);
                if (res_o == NULL) {
                    _Py_call_instrumentation_exc2(
                        tstate, PY_MONITORING_EVENT_C_RAISE,
                        frame, this_instr, callable_o, arg);
                }
                else {
                    int err = _Py_call_instrumentation_2args(
                        tstate, PY_MONITORING_EVENT_C_RETURN,
                        frame, this_instr, callable_o, arg);
                    if (err < 0) {
                        Py_CLEAR(res_o);
                    }
                }
            }
            DECREF_INPUTS();
            ERROR_IF(res_o == NULL);
            res = PyStackRef_FromPyObjectSteal(res_o);
        }

        override op(_DO_CALL_FUNCTION_EX, (func_st, null, callargs_st, kwargs_st -- result)) {
            (void)null;
            PyObject *func = PyStackRef_AsPyObjectBorrow(func_st);

            // DICT_MERGE is called before this opcode if there are kwargs.
            // It converts all dict subtypes in kwargs into regular dicts.
            EVAL_CALL_STAT_INC_IF_FUNCTION(EVAL_CALL_FUNCTION_EX, func);
            PyObject *result_o;
            assert(!_PyErr_Occurred(tstate));
            if (opcode == INSTRUMENTED_CALL_FUNCTION_EX) {
                PyObject *callargs = PyStackRef_AsPyObjectBorrow(callargs_st);
                PyObject *kwargs = PyStackRef_AsPyObjectBorrow(kwargs_st);
                assert(kwargs == NULL || PyDict_CheckExact(kwargs));
                assert(PyTuple_CheckExact(callargs));
                PyObject *arg = PyTuple_GET_SIZE(callargs) > 0 ?
                    PyTuple_GET_ITEM(callargs, 0) : &_PyInstrumentation_MISSING;
                int err = _Py_call_instrumentation_2args(
                    tstate, PY_MONITORING_EVENT_CALL,
                    frame, this_instr, func, arg);
                if (err) {
                    ERROR_NO_POP();
                }
                result_o = PyObject_Call(func, callargs, kwargs);

                if (!PyFunction_Check(func) && !PyMethod_Check(func)) {
                    if (result_o == NULL) {
                        _Py_call_instrumentation_exc2(
                            tstate, PY_MONITORING_EVENT_C_RAISE,
                            frame, this_instr, func, arg);
                    }
                    else {
                        int err = _Py_call_instrumentation_2args(
                            tstate, PY_MONITORING_EVENT_C_RETURN,
                            frame, this_instr, func, arg);
                        if (err < 0) {
                            Py_CLEAR(result_o);
                        }
                    }
                }
            }
            else {
                if (Py_TYPE(func) == &PyFunction_Type &&
                    !IS_PEP523_HOOKED(tstate) &&
                    ((PyFunctionObject *)func)->vectorcall == _PyFunction_Vectorcall) {
                    PyObject *callargs = PyStackRef_AsPyObjectSteal(callargs_st);
                    assert(PyTuple_CheckExact(callargs));
                    PyObject *kwargs = PyStackRef_IsNull(kwargs_st) ? NULL : PyStackRef_AsPyObjectSteal(kwargs_st);
                    assert(kwargs == NULL || PyDict_CheckExact(kwargs));
                    Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
                    int code_flags = ((PyCodeObject *)PyFunction_GET_CODE(func))->co_flags;
                    PyObject *locals = code_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(func));

                    _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit_Ex(
                        tstate, func_st, locals,
                        nargs, callargs, kwargs, frame);
                    // Need to sync the stack since we exit with DISPATCH_INLINED.
                    INPUTS_DEAD();
                    SYNC_SP();
                    if (new_frame == NULL) {
                        ERROR_NO_POP();
                    }
                    assert(INSTRUCTION_SIZE == 1);
                    frame->return_offset = 1;
                    DISPATCH_INLINED(new_frame);
                }
                PyObject *callargs = PyStackRef_AsPyObjectBorrow(callargs_st);
                assert(PyTuple_CheckExact(callargs));
                PyObject *kwargs = PyStackRef_AsPyObjectBorrow(kwargs_st);
                assert(kwargs == NULL || PyDict_CheckExact(kwargs));
                result_o = PyObject_Call(func, callargs, kwargs);
            }
            PyStackRef_XCLOSE(kwargs_st);
            PyStackRef_CLOSE(callargs_st);
            DEAD(null);
            PyStackRef_CLOSE(func_st);
            ERROR_IF(result_o == NULL);
            result = PyStackRef_FromPyObjectSteal(result_o);
        }

        override op(_SEND, (receiver, v -- receiver, retval)) {
            PyObject *receiver_o = PyStackRef_AsPyObjectBorrow(receiver);
            PyObject *retval_o;
            assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
            if (!IS_PEP523_HOOKED(tstate) &&
                (Py_TYPE(receiver_o) == &PyGen_Type || Py_TYPE(receiver_o) == &PyCoro_Type) &&
                ((PyGenObject *)receiver_o)->gi_frame_state < FRAME_EXECUTING)
            {
                PyGenObject *gen = (PyGenObject *)receiver_o;
                _PyInterpreterFrame *gen_frame = &gen->gi_iframe;
                _PyFrame_StackPush(gen_frame, PyStackRef_MakeHeapSafe(v));
                DEAD(v);
                SYNC_SP();
                gen->gi_frame_state = FRAME_EXECUTING;
                gen->gi_exc_state.previous_item = tstate->exc_info;
                tstate->exc_info = &gen->gi_exc_state;
                assert(INSTRUCTION_SIZE + oparg <= UINT16_MAX);
                frame->return_offset = (uint16_t)(INSTRUCTION_SIZE + oparg);
                assert(gen_frame->previous == NULL);
                gen_frame->previous = frame;
                DISPATCH_INLINED(gen_frame);
            }
            if (PyStackRef_IsNone(v) && PyIter_Check(receiver_o)) {
                retval_o = Py_TYPE(receiver_o)->tp_iternext(receiver_o);
            }
            else {
                retval_o = PyObject_CallMethodOneArg(receiver_o,
                                                     &_Py_ID(send),
                                                     PyStackRef_AsPyObjectBorrow(v));
            }
            if (retval_o == NULL) {
                int matches = _PyErr_ExceptionMatches(tstate, PyExc_StopIteration);
                if (matches) {
                    _PyEval_MonitorRaise(tstate, frame, this_instr);
                }
                int err = _PyGen_FetchStopIterationValue(&retval_o);
                if (err == 0) {
                    assert(retval_o != NULL);
                    JUMPBY(oparg);
                }
                else {
                    PyStackRef_CLOSE(v);
                    ERROR_IF(true);
                }
            }
            PyStackRef_CLOSE(v);
            retval = PyStackRef_FromPyObjectSteal(retval_o);
        }

        override inst(GET_ANEXT, (aiter -- aiter, awaitable)) {
            // CX: Use modified version of _PyEval_GetANext to handle JIT generators
            PyObject *awaitable_o = Ci_PyEval_GetANext(PyStackRef_AsPyObjectBorrow(aiter));
            if (awaitable_o == NULL) {
                ERROR_NO_POP();
            }
            awaitable = PyStackRef_FromPyObjectSteal(awaitable_o);
        }

        override inst(GET_AWAITABLE, (iterable -- iter)) {
            // CX: Use modified version of _PyEval_GetAwaitable to handle JIT generators
            PyObject *iter_o = Ci_PyEval_GetAwaitable(PyStackRef_AsPyObjectBorrow(iterable), oparg);
            PyStackRef_CLOSE(iterable);
            ERROR_IF(iter_o == NULL);
            iter = PyStackRef_FromPyObjectSteal(iter_o);
        }


// END BYTECODES //

    }
 dispatch_opcode:
 error:
 exception_unwind:
 exit_unwind:
 handle_eval_breaker:
 resume_frame:
 start_frame:
 unbound_local_error:
    ;
}

// Future families go below this point //
