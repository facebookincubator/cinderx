// clang-format off

// Copyright (c) Meta Platforms, Inc. and affiliates.

// Overrides for cinder-specific bytecode (Python 3.12)

// See third-party/python/3.12/Python/bytecodes.c
// - Instruction definitions between // BEGIN BYTECODES and // END BYTECODES
// - Dummy C code copied from bytecodes.c to keep editors happy.

#include "Python.h"
#include "pycore_abstract.h"      // _PyIndex_Check()
#include "pycore_call.h"          // _PyObject_FastCallDictTstate()
#include "pycore_ceval.h"         // _PyEval_SignalAsyncExc()
#include "pycore_code.h"
#include "pycore_function.h"
#include "pycore_import.h"        // _PyImport_ImportName()
#include "pycore_intrinsics.h"
#include "pycore_lazyimport.h"    // PyLazyImport_CheckExact()
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_instruments.h"
#include "pycore_object.h"        // _PyObject_GC_TRACK()
#include "pycore_moduleobject.h"  // PyModuleObject
#include "pycore_opcode.h"        // EXTRA_CASES
#include "pycore_pyerrors.h"      // _PyErr_GetRaisedException()
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_range.h"         // _PyRangeIterObject
#include "pycore_sliceobject.h"   // _PyBuildSlice_ConsumeRefs
#include "pycore_sysmodule.h"     // _PySys_Audit()
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_typeobject.h"    // _PySuper_Lookup()
#include "pycore_emscripten_signal.h"  // _Py_CHECK_EMSCRIPTEN_SIGNALS

#include "pycore_dict.h"
#include "dictobject.h"
#include "pycore_frame.h"
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"         // struct PyMemberDef, T_OFFSET_EX

#define USE_COMPUTED_GOTOS 0
#include "ceval_macros.h"

/* Flow control macros */
#define DEOPT_IF(cond, instname) ((void)0)
#define ERROR_IF(cond, labelname) ((void)0)
#define GO_TO_INSTRUCTION(instname) ((void)0)
#define PREDICT(opname) ((void)0)

#define inst(name, ...) case name:
#define op(name, ...) /* NAME is ignored */
#define macro(name) static int MACRO_##name
#define super(name) static int SUPER_##name
#define family(name, ...) static int family_##name

// Dummy variables for stack effects.
static PyObject *value, *value1, *value2, *left, *right, *res, *sum, *prod, *sub;
static PyObject *container, *start, *stop, *v, *lhs, *rhs, *res2;
static PyObject *list, *tuple, *dict, *owner, *set, *str, *tup, *map, *keys;
static PyObject *exit_func, *lasti, *val, *retval, *obj, *iter;
static PyObject *aiter, *awaitable, *iterable, *w, *exc_value, *bc, *locals;
static PyObject *orig, *excs, *update, *b, *fromlist, *level, *from;
static PyObject **pieces, **values;
static size_t jump;
// Dummy variables for cache effects
static uint16_t invert, counter, index, hint;
static uint32_t type_version;

static PyObject *
dummy_func(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame,
    unsigned char opcode,
    unsigned int oparg,
    _PyCFrame cframe,
    _Py_CODEUNIT *next_instr,
    PyObject **stack_pointer,
    PyObject *kwnames,
    int throwflag,
    binaryfunc binary_ops[],
    PyObject *args[]
)
{
    // Dummy labels.
    pop_1_error:
    // Dummy locals.
    PyObject *annotations;
    PyObject *attrs;
    PyObject *bottom;
    PyObject *callable;
    PyObject *callargs;
    PyObject *closure;
    PyObject *codeobj;
    PyObject *cond;
    PyObject *defaults;
    PyObject *descr;
    _PyInterpreterFrame  entry_frame;
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
    int values_or_none;

    switch (opcode) {

// BEGIN BYTECODES //

        override inst(GET_AWAITABLE, (iterable -- iter)) {
            // CX: Changed from _PyCoro_GetAwaitableIter
            iter = JitCoro_GetAwaitableIter(iterable);

            if (iter == NULL) {
                format_awaitable_error(tstate, Py_TYPE(iterable), oparg);
            }

            DECREF_INPUTS();

            // CX: Added JitCoro_CheckExact check
            if (iter != NULL && (PyCoro_CheckExact(iter) || JitCoro_CheckExact(iter))) {
                // CX: Changed from _PyGen_yf
                PyObject *yf = JitGen_yf((PyGenObject*)iter);
                if (yf != NULL) {
                    /* `iter` is a coroutine object that is being
                       awaited, `yf` is a pointer to the current awaitable
                       being awaited on. */
                    Py_DECREF(yf);
                    Py_CLEAR(iter);
                    _PyErr_SetString(tstate, PyExc_RuntimeError,
                                     "coroutine is being awaited already");
                    /* The code below jumps to `error` if `iter` is NULL. */
                }
            }

            ERROR_IF(iter == NULL, error);

            PREDICT(LOAD_CONST);
        }

        override inst(GET_ANEXT, (aiter -- aiter, awaitable)) {
            unaryfunc getter = NULL;
            PyObject *next_iter = NULL;
            PyTypeObject *type = Py_TYPE(aiter);

            if (PyAsyncGen_CheckExact(aiter)) {
                awaitable = type->tp_as_async->am_anext(aiter);
                if (awaitable == NULL) {
                    goto error;
                }
            } else {
                if (type->tp_as_async != NULL){
                    getter = type->tp_as_async->am_anext;
                }

                if (getter != NULL) {
                    next_iter = (*getter)(aiter);
                    if (next_iter == NULL) {
                        goto error;
                    }
                }
                else {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'async for' requires an iterator with "
                                  "__anext__ method, got %.100s",
                                  type->tp_name);
                    goto error;
                }

                // CX: Changed from _PyCoro_GetAwaitableIter
                awaitable = JitCoro_GetAwaitableIter(next_iter);
                if (awaitable == NULL) {
                    _PyErr_FormatFromCause(
                        PyExc_TypeError,
                        "'async for' received an invalid object "
                        "from __anext__: %.100s",
                        Py_TYPE(next_iter)->tp_name);

                    Py_DECREF(next_iter);
                    goto error;
                } else {
                    Py_DECREF(next_iter);
                }
            }

            PREDICT(LOAD_CONST);
        }

        override inst(GET_YIELD_FROM_ITER, (iterable -- iter)) {
            /* before: [obj]; after [getiter(obj)] */
            // CX: Added JitCoro_CheckExact check
            if (JitCoro_CheckExact(iterable) || PyCoro_CheckExact(iterable)) {
                /* `iterable` is a coroutine */
                if (!(frame->f_code->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE))) {
                    /* and it is used in a 'yield from' expression of a
                       regular generator. */
                    _PyErr_SetString(tstate, PyExc_TypeError,
                                     "cannot 'yield from' a coroutine object "
                                     "in a non-coroutine generator");
                    goto error;
                }
                iter = iterable;
            }
            // CX: Added JitGen_CheckExact check
            else if (JitGen_CheckExact(iterable) || PyGen_CheckExact(iterable)) {
                iter = iterable;
            }
            else {
                /* `iterable` is not a generator. */
                iter = PyObject_GetIter(iterable);
                if (iter == NULL) {
                    goto error;
                }
                DECREF_INPUTS();
            }
            PREDICT(LOAD_CONST);
        }

       override inst(SEND_GEN, (unused/1, receiver, v -- receiver, unused)) {
            DEOPT_IF(tstate->interp->eval_frame, SEND);
            PyGenObject *gen = (PyGenObject *)receiver;
            DEOPT_IF(Py_TYPE(gen) != &PyGen_Type &&
                     Py_TYPE(gen) != &PyCoro_Type, SEND);
            DEOPT_IF(gen->gi_frame_state >= FRAME_EXECUTING, SEND);
            STAT_INC(SEND, hit);
            if ((frame->owner == FRAME_OWNED_BY_GENERATOR) &&
                (frame->f_code->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR))) {
                Ci_PyAwaitable_SetAwaiter(receiver, (PyObject *) _PyFrame_GetGenerator(frame));
            }
            _PyInterpreterFrame *gen_frame = (_PyInterpreterFrame *)gen->gi_iframe;
            frame->return_offset = oparg;
            STACK_SHRINK(1);
            _PyFrame_StackPush(gen_frame, v);
            gen->gi_frame_state = FRAME_EXECUTING;
            gen->gi_exc_state.previous_item = tstate->exc_info;
            tstate->exc_info = &gen->gi_exc_state;
            JUMPBY(INLINE_CACHE_ENTRIES_SEND);
            DISPATCH_INLINED(gen_frame);
        }

        override inst(WITH_EXCEPT_START, (exit_func, lasti, unused, val -- exit_func, lasti, unused, val, res)) {
            /* At the top of the stack are 4 values:
               - val: TOP = exc_info()
               - unused: SECOND = previous exception
               - lasti: THIRD = lasti of exception in exc_info()
               - exit_func: FOURTH = the context.__exit__ bound method
               We call FOURTH(type(TOP), TOP, GetTraceback(TOP)).
               Then we push the __exit__ return value.
            */
            PyObject *exc, *tb;

            assert(val && PyExceptionInstance_Check(val));
            exc = PyExceptionInstance_Class(val);
            PyObject *original_tb = tb = PyException_GetTraceback(val);
            // Modified from stock CPython to not free the traceback until after the vectorcall.
            // If the receiver of the traceback doesn't incref it, and the traceback is replaced
            // on the exception, then there may no longer be any references keeping it alive.

            if (tb == NULL) {
                tb = Py_None;
            }
            assert(PyLong_Check(lasti));
            (void)lasti; // Shut up compiler warning if asserts are off
            PyObject *stack[4] = {NULL, exc, val, tb};
            res = PyObject_Vectorcall(exit_func, stack + 1,
                    3 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
            Py_XDECREF(original_tb);
            ERROR_IF(res == NULL, error);
        }

        override inst(EXTENDED_ARG, ( -- )) {
            opcode = next_instr->op.code;
            oparg = oparg << 8 | next_instr->op.arg;
            PRE_DISPATCH_GOTO();
            DISPATCH_GOTO();
        }

        override inst(MAP_ADD, (key, value --)) {
            PyObject *dict = PEEK(oparg + 2);  // key, value are still on the stack
            assert(PyDict_CheckExact(dict) || Ci_CheckedDict_Check(dict));
            /* dict[key] = value */
            int set = Ci_DictOrChecked_SetItem(dict, key, value);
            DECREF_INPUTS();
            ERROR_IF(set != 0, error);
            PREDICT(JUMP_BACKWARD);
        }

        override inst(LIST_APPEND, (list, unused[oparg-1], v -- list, unused[oparg-1])) {
            int append = Ci_ListOrCheckedList_Append((PyListObject*)list, v);
            Py_DECREF(v);
            ERROR_IF(append < 0, error);
            PREDICT(JUMP_BACKWARD);
        }

        inst(POP_JUMP_IF_ZERO, (cond --)) {
            int is_nonzero = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (!is_nonzero) {
                JUMPBY(oparg);
            }
        }

        inst(POP_JUMP_IF_NONZERO, (cond --)) {
            int is_nonzero = PyObject_IsTrue(cond);
            Py_DECREF(cond);
            if (is_nonzero) {
                JUMPBY(oparg);
            }
        }

        inst(LOAD_ITERABLE_ARG, (tup -- element, tup)) {
            int idx = oparg;
            if (!PyTuple_CheckExact(tup)) {
                if (tup->ob_type->tp_iter == NULL && !PySequence_Check(tup)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "argument after * "
                        "must be an iterable, not %.200s",
                        tup->ob_type->tp_name);
                    goto error;
                }
                Py_SETREF(tup, PySequence_Tuple(tup));
                ERROR_IF(tup == NULL, error);
            }
            element = PyTuple_GetItem(tup, idx);
            if (element == NULL) {
                goto error;
            }
            Py_INCREF(element);
        }

        inst(LOAD_MAPPING_ARG, (defaultval if (oparg == 3), mapping, name -- value)) {
            if (!PyDict_Check(mapping) && !Ci_CheckedDict_Check(mapping)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "argument after ** "
                    "must be a dict, not %.200s",
                    mapping->ob_type->tp_name);
                goto error;
            }

            value = PyDict_GetItemWithError(mapping, name);
            if (value == NULL) {
                if (_PyErr_Occurred(tstate)) {
                    goto error;
                } else if (oparg == 2) {
                    PyErr_Format(PyExc_TypeError, "missing argument %U", name);
                    assert(defaultval == NULL);
                    goto error;
                } else {
                    /* Default value is on the stack */
                    value = defaultval;
                }
            }

            Py_INCREF(value);
            DECREF_INPUTS();
        }

        inst(REFINE_TYPE, (unused -- unused)) {
        }

        family(load_super_attr, INLINE_CACHE_ENTRIES_LOAD_SUPER_ATTR) = {
            LOAD_SUPER_ATTR,
            LOAD_SUPER_ATTR_ATTR,
            LOAD_SUPER_ATTR_METHOD,
        };

        family(tp_alloc, TP_ALLOC_CACHE_SIZE) = {
            TP_ALLOC,
            TP_ALLOC_CACHED,
        };

        inst(TP_ALLOC, (unused/2 -- inst)) {
            int optional;
            int exact;
            PyTypeObject *type = _PyClassLoader_ResolveType(
                GETITEM(frame->f_code->co_consts, oparg), &optional, &exact);
            assert(!optional);
            ERROR_IF(type == NULL, error);

            inst = type->tp_alloc(type, 0);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                int32_t index = _PyClassLoader_CacheValue((PyObject *)type);
                if (index >= 0) {
                    int32_t *cache = (int32_t *)next_instr;
                    *cache = index;
                    _Ci_specialize(next_instr, TP_ALLOC_CACHED);
                }
            }
 #endif
            Py_DECREF(type);
            ERROR_IF(inst == NULL, error);
        }

        inst(TP_ALLOC_CACHED, (cache/2 -- inst)) {
            PyTypeObject *type = (PyTypeObject *)_PyClassLoader_GetCachedValue(cache);
            DEOPT_IF(type == NULL, TP_ALLOC);

            inst = type->tp_alloc(type, 0);
            Py_DECREF(type);
            ERROR_IF(inst == NULL, error);
        }

        inst(LOAD_LOCAL, (-- value))  {
            int index = _PyLong_AsInt(PyTuple_GET_ITEM(GETITEM(frame->f_code->co_consts, oparg), 0));

            value = GETLOCAL(index);
            if (value == NULL) {
                // Primitive values are default initialized to zero, so they don't
                // need to be defined. We should consider stop doing that as it can
                // cause compatibility issues when the same code runs statically and
                // non statically.
                value = PyLong_FromLong(0);
                SETLOCAL(index, value); /* will steal the ref */
            }
            Py_INCREF(value);
        }

        family(store_local, STORE_LOCAL_CACHE_SIZE) = {
            STORE_LOCAL,
            STORE_LOCAL_CACHED,
        };

        inst(STORE_LOCAL, (unused/1, val -- )) {
            PyObject* local = GETITEM(frame->f_code->co_consts, oparg);
            int index = _PyLong_AsInt(PyTuple_GET_ITEM(local, 0));
            int type =
                _PyClassLoader_ResolvePrimitiveType(PyTuple_GET_ITEM(local, 1));

            if (type < 0) {
                goto error;
            }

            if (type == TYPED_DOUBLE) {
                SETLOCAL(index, val);
            } else {
                Py_ssize_t ival = unbox_primitive_int_and_decref(val);
                SETLOCAL(index, box_primitive(type, ival));
            }

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                if (index < INT8_MAX && type < INT8_MAX) {
                    int16_t *cache = (int16_t*)next_instr;
                    *cache = (index << 8) | type;
                    _Ci_specialize(next_instr, STORE_LOCAL_CACHED);
                }
            }
#endif
        }

        inst(STORE_LOCAL_CACHED, (cache/1, val -- )) {
            int type = cache & 0xFF;
            int idx = cache >> 8;
            if (type == TYPED_DOUBLE) {
              SETLOCAL(idx, val);
            } else {
              Py_ssize_t value = unbox_primitive_int_and_decref(val);
              SETLOCAL(idx, box_primitive(type, value));
            }
        }

        family(load_super_attr, INLINE_CACHE_ENTRIES_LOAD_FIELD) = {
            LOAD_FIELD,
            LOAD_OBJ_FIELD,
            LOAD_PRIMITIVE_FIELD,
        };

        inst(LOAD_FIELD, (unused/2, self -- value)) {
            PyObject* field = GETITEM(frame->f_code->co_consts, oparg);
            int field_type;
            Py_ssize_t offset =
                _PyClassLoader_ResolveFieldOffset(field, &field_type);
            if (offset == -1) {
                goto error;
            }

            if (field_type == TYPED_OBJECT) {
                value = *FIELD_OFFSET(self, offset);
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
                if (adaptive_enabled) {
                    if (offset < INT32_MAX) {
                        int32_t *cache = (int32_t*)next_instr;
                        *cache = offset;
                        _Ci_specialize(next_instr, LOAD_OBJ_FIELD);
                    }
                }
#endif

                if (value == NULL) {
                    PyObject* name =
                        PyTuple_GET_ITEM(field, PyTuple_GET_SIZE(field) - 1);
                    PyErr_Format(
                        PyExc_AttributeError,
                        "'%.50s' object has no attribute '%U'",
                        Py_TYPE(self)->tp_name,
                        name);
                    goto error;
                }
                Py_INCREF(value);
            } else {
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
                if (adaptive_enabled) {
                    if (offset <= INT32_MAX >> 8) {
                        assert(field_type < 0xff);
                        int32_t *cache = (int32_t*)next_instr;
                        *cache = offset << 8 | field_type;
                        _Ci_specialize(next_instr, LOAD_PRIMITIVE_FIELD);
                    }
                }
#endif

                value = load_field(field_type, (char*)FIELD_OFFSET(self, offset));
                if (value == NULL) {
                    goto error;
                }
            }
            Py_DECREF(self);
        }

        inst(LOAD_OBJ_FIELD, (offset/2, self -- value)) {
            PyObject** addr = FIELD_OFFSET(self, offset);
            value = *addr;

            if (value == NULL) {
              PyErr_Format(
                  PyExc_AttributeError,
                  "'%.50s' object has no attribute",
                  Py_TYPE(self)->tp_name);
              goto error;
            }
            Py_INCREF(value);
            DECREF_INPUTS();
        }

        inst(LOAD_PRIMITIVE_FIELD, (field_type/2, self -- value)) {
            value =
                load_field(field_type & 0xff, ((char*)TOP()) + (field_type >> 8));

            DECREF_INPUTS();
            ERROR_IF(value == NULL, error);
        }

        family(load_super_attr, INLINE_CACHE_ENTRIES_STORE_FIELD) = {
            STORE_FIELD,
            STORE_OBJ_FIELD,
            STORE_PRIMITIVE_FIELD,
        };

        inst(STORE_FIELD, (unused/2, value, self --)) {
            PyObject* field = GETITEM(frame->f_code->co_consts, oparg);
            int field_type;
            Py_ssize_t offset =
                _PyClassLoader_ResolveFieldOffset(field, &field_type);
            if (offset == -1) {
                goto error;
            }

            PyObject** addr = FIELD_OFFSET(self, offset);

            if (field_type == TYPED_OBJECT) {
                Py_XDECREF(*addr);
                *addr = value;
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
                if (adaptive_enabled) {
                    if (offset <= INT32_MAX) {
                        int32_t *cache = (int32_t*)next_instr;
                        *cache = offset;
                        _Ci_specialize(next_instr, STORE_OBJ_FIELD);
                    }
                }
#endif
            } else {
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
                if (adaptive_enabled) {
                    if (offset <= INT32_MAX >> 8) {
                        assert(field_type < 0xff);
                        int32_t *cache = (int32_t*)next_instr;
                        *cache = offset << 8 | field_type;
                        _Ci_specialize(next_instr, STORE_PRIMITIVE_FIELD);
                    }
                }
#endif
                store_field(field_type, (char*)addr, value);
            }
            Py_DECREF(self);
        }

        inst(STORE_OBJ_FIELD, (offset/2, value, self --)) {
            PyObject** addr = FIELD_OFFSET(self, offset);
            Py_XDECREF(*addr);
            *addr = value;
            Py_DECREF(self);
        }

        inst(STORE_PRIMITIVE_FIELD, (field_type/2, value, self --)) {
            PyObject** addr = FIELD_OFFSET(self, (field_type >> 8));
            store_field(field_type & 0xff, (char*)addr, value);
            Py_DECREF(self);
        }

        family(tp_alloc, CAST_CACHE_SIZE) = {
            CAST,
            CAST_CACHED,
        };

        inst(CAST, (unused/2, val -- res)) {
            int optional;
            int exact;
            PyTypeObject* type = _PyClassLoader_ResolveType(
                GETITEM(frame->f_code->co_consts, oparg), &optional, &exact);
            if (type == NULL) {
                goto error;
            }
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                int32_t index = _PyClassLoader_CacheValue((PyObject *)type);
                if (index >= 0 && index <= INT32_MAX >> 2) {
                    int32_t *cache = (int32_t*)next_instr;
                    *cache = (int32_t)(index << 2) | (exact << 1) | optional;
                    _Ci_specialize(next_instr, CAST_CACHED);
                }
            }
#endif
            if (!_PyObject_TypeCheckOptional(val, type, optional, exact)) {
                CAST_COERCE_OR_ERROR(val, type, exact);
            }

            res = val;
            Py_DECREF(type);
        }

        inst(CAST_CACHED, (cache/2, val -- res)) {
            PyTypeObject* type = (PyTypeObject *)_PyClassLoader_GetCachedValue(cache >> 2);
            DEOPT_IF(type == NULL, CAST);

            int optional = cache & 0x01;
            int exact = (cache >> 1) & 0x01;
            if (!_PyObject_TypeCheckOptional(val, type, optional, exact)) {
                CAST_COERCE_OR_ERROR(val, type, exact);
            }
            res = val;
            Py_DECREF(type);
        }

        inst(SEQUENCE_GET, (sequence, idx -- item)) {
            Py_ssize_t val = (Py_ssize_t)PyLong_AsVoidPtr(idx);

            if (val == -1 && _PyErr_Occurred(tstate)) {
                goto error;
            }

            // Adjust index
            if (val < 0) {
                val += Py_SIZE(sequence);
            }

            oparg &= ~SEQ_SUBSCR_UNCHECKED;

            if (oparg == SEQ_LIST) {
                item = PyList_GetItem(sequence, val);
                if (item == NULL) {
                    goto error;
                }
                Py_INCREF(item);
            } else if (oparg == SEQ_LIST_INEXACT) {
                if (PyList_CheckExact(sequence) ||
                    Py_TYPE(sequence)->tp_as_sequence->sq_item ==
                        PyList_Type.tp_as_sequence->sq_item) {
                    item = PyList_GetItem(sequence, val);
                    if (item == NULL) {
                        goto error;
                    }
                    Py_INCREF(item);
                } else {
                    item = PyObject_GetItem(sequence, idx);
                    if (item == NULL) {
                        goto error;
                    }
                }
            } else if (oparg == SEQ_CHECKED_LIST) {
                item = Ci_CheckedList_GetItem(sequence, val);
                if (item == NULL) {
                    goto error;
                }
            } else if (oparg == SEQ_ARRAY_INT64) {
                item = _Ci_StaticArray_Get(sequence, val);
                if (item == NULL) {
                    goto error;
                }
            } else {
                PyErr_Format(
                    PyExc_SystemError, "bad oparg for SEQUENCE_GET: %d", oparg);
                goto error;
            }

            DECREF_INPUTS();
        }

        inst(SEQUENCE_SET, (v, sequence, subscr -- )) {
            int err;

            Py_ssize_t idx = (Py_ssize_t)PyLong_AsVoidPtr(subscr);

            if (idx == -1 && _PyErr_Occurred(tstate)) {
                goto error;
            }

            // Adjust index
            if (idx < 0) {
                idx += Py_SIZE(sequence);
            }

            if (oparg == SEQ_LIST) {
                Py_INCREF(v);   // PyList_SetItem steals the reference
                err = PyList_SetItem(sequence, idx, v);

                if (err != 0) {
                    Py_DECREF(v);
                    goto error;
                }
            } else if (oparg == SEQ_LIST_INEXACT) {
                if (PyList_CheckExact(sequence) ||
                    Py_TYPE(sequence)->tp_as_sequence->sq_ass_item ==
                        PyList_Type.tp_as_sequence->sq_ass_item) {
                    Py_INCREF(v);   // PyList_SetItem steals the reference
                    err = PyList_SetItem(sequence, idx, v);

                    if (err != 0) {
                        Py_DECREF(v);
                        goto error;
                    }
                } else {
                    err = PyObject_SetItem(sequence, subscr, v);
                    if (err != 0) {
                        goto error;
                    }
                }
            } else if (oparg == SEQ_ARRAY_INT64) {
                err = _Ci_StaticArray_Set(sequence, idx, v);

                if (err != 0) {
                    goto error;
                }
            } else {
                PyErr_Format(
                    PyExc_SystemError, "bad oparg for SEQUENCE_SET: %d", oparg);
                goto error;
            }

            DECREF_INPUTS();
        }

        inst(LIST_DEL, (list, subscr -- )) {
            int err;

            Py_ssize_t idx = PyLong_AsLong(subscr);

            if (idx == -1 && _PyErr_Occurred(tstate)) {
                goto error;
            }

            err = PyList_SetSlice(list, idx, idx + 1, NULL);
            DECREF_INPUTS();
            ERROR_IF(err != 0, error);
        }

        inst(FAST_LEN, (collection -- length)) {
            int inexact = oparg & FAST_LEN_INEXACT;
            oparg &= ~FAST_LEN_INEXACT;
            assert(FAST_LEN_LIST <= oparg && oparg <= FAST_LEN_STR);
            if (inexact) {
                if ((oparg == FAST_LEN_LIST && PyList_CheckExact(collection)) ||
                    (oparg == FAST_LEN_DICT && PyDict_CheckExact(collection)) ||
                    (oparg == FAST_LEN_SET && PyAnySet_CheckExact(collection)) ||
                    (oparg == FAST_LEN_TUPLE && PyTuple_CheckExact(collection)) ||
                    (oparg == FAST_LEN_ARRAY &&
                    PyStaticArray_CheckExact(collection)) ||
                    (oparg == FAST_LEN_STR && PyUnicode_CheckExact(collection))) {
                    inexact = 0;
                }
            }
            if (inexact) {
                Py_ssize_t res = PyObject_Size(collection);
                if (res >= 0) {
                    length = PyLong_FromSsize_t(res);
                } else {
                    length = NULL;
                }
            } else if (oparg == FAST_LEN_DICT) {
                length = PyLong_FromLong(((PyDictObject*)collection)->ma_used);
            } else if (oparg == FAST_LEN_SET) {
                length = PyLong_FromLong(((PySetObject*)collection)->used);
            } else {
                // lists, tuples, arrays are all PyVarObject and use ob_size
                length = PyLong_FromLong(Py_SIZE(collection));
            }
            Py_DECREF(collection);
            ERROR_IF(length == NULL, error);
        }

        inst(PRIMITIVE_BOX, (top -- res)) {
            res = sign_extend_primitive(top, oparg);
        }

        inst(PRIMITIVE_UNBOX, (top -- top)) {
            /* We always box values in the interpreter loop (they're only
            * unboxed in the JIT where they can't be introspected at runtime),
            * so this just does overflow checking here. Oparg indicates the
            * type of the unboxed value. */
            if (PyLong_CheckExact(top)) {
                size_t value;
                if (!_PyClassLoader_OverflowCheck(top, oparg, &value)) {
                    PyErr_SetString(PyExc_OverflowError, "int overflow");
                    goto error;
                }
            }
        }

        inst(PRIMITIVE_UNARY_OP, (val -- res)) {
            res = primitive_unary_op(val, oparg);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(CONVERT_PRIMITIVE, (val -- res)) {
            Py_ssize_t from_type = oparg & 0xFF;
            Py_ssize_t to_type = oparg >> 4;
            Py_ssize_t extend_sign =
                (from_type & TYPED_INT_SIGNED) && (to_type & TYPED_INT_SIGNED);
            int size = to_type >> 1;
            size_t ival = (size_t)PyLong_AsVoidPtr(val);

            ival &= trunc_masks[size];

            // Extend the sign if needed
            if (extend_sign != 0 && (ival & signed_bits[size])) {
                ival |= (signex_masks[size]);
            }

            res = PyLong_FromSize_t(ival);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(PRIMITIVE_BINARY_OP, (l, r -- res)) {
            res = primitive_binary_op(l, r, oparg);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(PRIMITIVE_COMPARE_OP, (l, r -- res)) {
            res = primitive_compare_op(l, r, oparg);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(PRIMITIVE_LOAD_CONST, ( -- res)) {
            res = PyTuple_GET_ITEM(GETITEM(frame->f_code->co_consts, oparg), 0);
            Py_INCREF(res);
        }

        inst(RETURN_PRIMITIVE, (retval --)) {
            /* In the interpreter, we always return a boxed int. We have a boxed
            * value on the stack already, but we may have to deal with sign
            * extension. */
            retval = sign_extend_primitive(retval, oparg);

            STACK_SHRINK(1);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            frame->prev_instr += frame->return_offset;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        inst(LOAD_TYPE, (instance -- type)) {
            type = (PyObject *)Py_TYPE(instance);
            Py_INCREF(type);
            DECREF_INPUTS();
        }

        inst(LOAD_CLASS, (-- type)) {
            PyObject* type_descr = GETITEM(frame->f_code->co_consts, oparg);
            int optional;
            int exact;
            type =
                (PyObject *)_PyClassLoader_ResolveType(type_descr, &optional, &exact);
            ERROR_IF(type == NULL, error);
        }

        family(tp_alloc, INVOKE_FUNCTION_CACHE_SIZE) = {
            INVOKE_FUNCTION,
            INVOKE_FUNCTION_CACHED,
        };

        inst(INVOKE_FUNCTION, (unused/4, args[invoke_function_args(frame->f_code->co_consts, oparg)] -- res)) {
            // We should move to encoding the number of args directly in the
            // opcode, right now pulling them out via invoke_function_args is a little
            // ugly.
            PyObject* value = GETITEM(frame->f_code->co_consts, oparg);
            int nargs = invoke_function_args(frame->f_code->co_consts, oparg);
            PyObject* target = PyTuple_GET_ITEM(value, 0);
            PyObject* container;
            PyObject* func = _PyClassLoader_ResolveFunction(target, &container);
            if (func == NULL) {
                goto error;
            }

            res = _PyObject_Vectorcall(func, args, nargs, NULL);
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                if (_PyClassLoader_IsImmutable(container)) {
                    /* frozen type, we don't need to worry about indirecting */
                    int32_t index = _PyClassLoader_CacheValue(func);
                    if (index >= 0) {
                        int32_t *cache = (int32_t*)next_instr;
                        *cache = index;
                        _Ci_specialize(next_instr, INVOKE_FUNCTION_CACHED);
                    }
                } else {
                    PyObject** funcptr = _PyClassLoader_ResolveIndirectPtr(target);
                    PyObject ***cache = (PyObject ***)next_instr;
                    *cache = funcptr;
                    _Ci_specialize(next_instr, INVOKE_INDIRECT_CACHED);
                }
            }
#endif
            Py_DECREF(func);
            Py_DECREF(container);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INVOKE_FUNCTION_CACHED,  (cache/4, args[invoke_function_args(frame->f_code->co_consts, oparg)] -- res)) {
            // It's assumed a 64-bit value is a pointer, but we want 64-bits for the indirect
            // version and just use an int for this cached version.
            PyObject* func = _PyClassLoader_GetCachedValue((int32_t)(intptr_t)cache);
            DEOPT_IF(func == NULL, INVOKE_FUNCTION);

            int nargs = invoke_function_args(frame->f_code->co_consts, oparg);

            res = _PyObject_Vectorcall(func, args, nargs, NULL);
            Py_DECREF(func);

            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INVOKE_INDIRECT_CACHED, (cache/4, args[invoke_function_args(frame->f_code->co_consts, oparg)] -- res)) {
            PyObject** funcref = (PyObject**)cache;
            int nargs = invoke_function_args(frame->f_code->co_consts, oparg);

            PyObject* func = *funcref;
            /* For indirect calls we just use _PyObject_Vectorcall, which will
            * handle non-vector call objects as well.  We expect in high-perf
            * situations to either have frozen types or frozen strict modules */
            DEOPT_IF(func == NULL, INVOKE_FUNCTION);

            res = _PyObject_Vectorcall(func, args, nargs, NULL);

            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(LOAD_METHOD_STATIC, (unused/2, self -- func, self)) {
            PyObject* value = GETITEM(frame->f_code->co_consts, oparg);
            PyObject* target = PyTuple_GET_ITEM(value, 0);
            int is_classmethod = _PyClassLoader_IsClassMethodDescr(value);

            Py_ssize_t slot = _PyClassLoader_ResolveMethod(target);
            if (slot == -1) {
                goto error;
            }

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                // We encode class method as the low bit hence the >> 1.
                if (slot < (INT32_MAX >> 1)) {
                    /* We smuggle in the information about whether the invocation was a
                    * classmethod in the low bit of the oparg. This is necessary, as
                    * without, the runtime won't be able to get the correct vtable from
                    * self when the type is passed in.
                    */
                    int32_t *cache = (int32_t*)next_instr;
                    *cache = load_method_static_cached_oparg(slot, is_classmethod);
                    _Ci_specialize(next_instr, LOAD_METHOD_STATIC_CACHED);
                }
            }
#endif

            _PyType_VTable* vtable;
            if (is_classmethod) {
                vtable = (_PyType_VTable*)(((PyTypeObject*)self)->tp_cache);
            } else {
                vtable = (_PyType_VTable*)self->ob_type->tp_cache;
            }

            assert(!PyErr_Occurred());
            StaticMethodInfo res =
                _PyClassLoader_LoadStaticMethod(vtable, slot, self);
            if (res.lmr_func == NULL) {
                goto error;
            }

            func = res.lmr_func;
        }

        inst(LOAD_METHOD_STATIC_CACHED, (cache/2, self -- func, self)) {
            bool is_classmethod =
                load_method_static_cached_oparg_is_classmethod(cache);
            Py_ssize_t slot = load_method_static_cached_oparg_slot(cache);

            _PyType_VTable* vtable;
            if (is_classmethod) {
                vtable = (_PyType_VTable*)(((PyTypeObject*)self)->tp_cache);
            } else {
                vtable = (_PyType_VTable*)self->ob_type->tp_cache;
            }

            assert(!PyErr_Occurred());
            StaticMethodInfo res =
                _PyClassLoader_LoadStaticMethod(vtable, slot, self);
            if (res.lmr_func == NULL) {
                goto error;
            }

            func = res.lmr_func;
        }

        inst(INVOKE_METHOD, (target, args[invoke_function_args(frame->f_code->co_consts, oparg) + 1] -- res)) {
            Py_ssize_t nargs = invoke_function_args(frame->f_code->co_consts, oparg) + 1;

            assert(!PyErr_Occurred());

            res = PyObject_Vectorcall(target, args, nargs, NULL);

            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(INVOKE_NATIVE, (args[invoke_native_args(frame->f_code->co_consts, oparg)] -- res)) {
            PyObject* value = GETITEM(frame->f_code->co_consts, oparg);
            assert(PyTuple_CheckExact(value));
            Py_ssize_t nargs = invoke_native_args(frame->f_code->co_consts, oparg);

            PyObject* target = PyTuple_GET_ITEM(value, 0);
            PyObject* name = PyTuple_GET_ITEM(target, 0);
            PyObject* symbol = PyTuple_GET_ITEM(target, 1);
            PyObject* signature = PyTuple_GET_ITEM(value, 1);

            res = _PyClassloader_InvokeNativeFunction(
                name, symbol, signature, args, nargs);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        family(load_super_attr, INLINE_CACHE_ENTRIES_BUILD_CHECKED_LIST) = {
            BUILD_CHECKED_LIST,
            BUILD_CHECKED_LIST_CACHED,
        };

        inst(BUILD_CHECKED_LIST, (unused/2, list_items[build_checked_obj_size(frame->f_code->co_consts, oparg)] -- list)) {
            PyObject* list_info = GETITEM(frame->f_code->co_consts, oparg);
            PyObject* list_type = PyTuple_GET_ITEM(list_info, 0);
            Py_ssize_t list_size = PyLong_AsLong(PyTuple_GET_ITEM(list_info, 1));

            int optional;
            int exact;
            PyTypeObject* type =
                _PyClassLoader_ResolveType(list_type, &optional, &exact);
            assert(!optional);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                int32_t index = _PyClassLoader_CacheValue((PyObject *)type);
                if (index >= 0) {
                    int32_t *cache = (int32_t*)next_instr;
                    *cache = index;
                    _Ci_specialize(next_instr, BUILD_CHECKED_LIST_CACHED);
                }
            }
#endif

            list = Ci_CheckedList_New(type, list_size);
            Py_DECREF(type);

            if (list == NULL) {
                goto error;
            }

            for (Py_ssize_t i = 0; i < list_size; i++) {
                Ci_ListOrCheckedList_SET_ITEM(list, i, list_items[i]);
            }
        }

        inst(BUILD_CHECKED_LIST_CACHED, (cache/2, list_items[build_checked_obj_size(frame->f_code->co_consts, oparg)] -- list)) {
            PyTypeObject *type = (PyTypeObject *)_PyClassLoader_GetCachedValue(cache);
            DEOPT_IF(type == NULL, BUILD_CHECKED_LIST);

            PyObject* list_info = GETITEM(frame->f_code->co_consts, oparg);
            Py_ssize_t list_size = PyLong_AsLong(PyTuple_GET_ITEM(list_info, 1));
            list = Ci_CheckedList_New(type, list_size);
            Py_DECREF(type);

            if (list == NULL) {
                goto error;
            }

            for (Py_ssize_t i = 0; i < list_size; i++) {
                Ci_ListOrCheckedList_SET_ITEM(list, i, list_items[i]);
            }
        }

        family(load_super_attr, INLINE_CACHE_ENTRIES_BUILD_CHECKED_MAP) = {
            BUILD_CHECKED_MAP,
            BUILD_CHECKED_MAP_CACHED,
        };

        inst(BUILD_CHECKED_MAP, (unused/2, map_items[build_checked_obj_size(frame->f_code->co_consts, oparg) * 2] -- map)) {
            PyObject* map_info = GETITEM(frame->f_code->co_consts, oparg);
            PyObject* map_type = PyTuple_GET_ITEM(map_info, 0);
            Py_ssize_t map_size = PyLong_AsLong(PyTuple_GET_ITEM(map_info, 1));

            int optional;
            int exact;
            PyTypeObject* type =
                _PyClassLoader_ResolveType(map_type, &optional, &exact);
            assert(!optional);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
            if (adaptive_enabled) {
                int32_t index = _PyClassLoader_CacheValue((PyObject *)type);
                if (index >= 0) {
                    int32_t *cache = (int32_t*)next_instr;
                    *cache = index;
                    _Ci_specialize(next_instr, BUILD_CHECKED_MAP_CACHED);
                }
            }
#endif

            map = Ci_CheckedDict_NewPresized(type, map_size);
            Py_DECREF(type);
            if (map == NULL) {
                goto error;
            }

            if (ci_build_dict(map_items, map_size, map) < 0) {
                Py_DECREF(map);
                map = NULL;
            }
            DECREF_INPUTS();
            ERROR_IF(map == NULL, error);
        }

        inst(BUILD_CHECKED_MAP_CACHED, (cache/2, map_items[build_checked_obj_size(frame->f_code->co_consts, oparg) * 2] -- map)) {
            PyTypeObject *type = (PyTypeObject *)_PyClassLoader_GetCachedValue(cache);
            DEOPT_IF(type == NULL, BUILD_CHECKED_MAP);

            PyObject* map_info = GETITEM(frame->f_code->co_consts, oparg);
            Py_ssize_t map_size = PyLong_AsLong(PyTuple_GET_ITEM(map_info, 1));

            map = Ci_CheckedDict_NewPresized(type, map_size);
            Py_DECREF(type);
            if (map == NULL) {
                goto error;
            }

            if (ci_build_dict(map_items, map_size, map) < 0) {
                Py_DECREF(map);
                map = NULL;
            }
            DECREF_INPUTS();
            ERROR_IF(map == NULL, error);
        }

        // return opcodes are modified to support updating adaptive_enabled after returning
        // from a Python -> Python call.
        override inst(RETURN_VALUE, (retval --)) {
            STACK_SHRINK(1);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            frame->prev_instr += frame->return_offset;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(INSTRUMENTED_RETURN_VALUE, (retval --)) {
            int err = _Py_call_instrumentation_arg(
                    tstate, PY_MONITORING_EVENT_PY_RETURN,
                    frame, next_instr-1, retval);
            if (err) goto error;
            STACK_SHRINK(1);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            frame->prev_instr += frame->return_offset;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(RETURN_CONST, (--)) {
            PyObject *retval = GETITEM(frame->f_code->co_consts, oparg);
            Py_INCREF(retval);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            frame->prev_instr += frame->return_offset;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(INSTRUMENTED_RETURN_CONST, (--)) {
            PyObject *retval = GETITEM(frame->f_code->co_consts, oparg);
            int err = _Py_call_instrumentation_arg(
                    tstate, PY_MONITORING_EVENT_PY_RETURN,
                    frame, next_instr-1, retval);
            if (err) goto error;
            Py_INCREF(retval);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            frame->prev_instr += frame->return_offset;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(INSTRUMENTED_YIELD_VALUE, (retval -- unused)) {
            assert(frame != &entry_frame);
            PyGenObject *gen = _PyFrame_GetGenerator(frame);
            gen->gi_frame_state = FRAME_SUSPENDED;
            _PyFrame_SetStackPointer(frame, stack_pointer - 1);
            int err = _Py_call_instrumentation_arg(
                    tstate, PY_MONITORING_EVENT_PY_YIELD,
                    frame, next_instr-1, retval);
            if (err) goto error;
            tstate->exc_info = gen->gi_exc_state.previous_item;
            gen->gi_exc_state.previous_item = NULL;
            _Py_LeaveRecursiveCallPy(tstate);
            _PyInterpreterFrame *gen_frame = frame;
            frame = cframe.current_frame = frame->previous;
            gen_frame->previous = NULL;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(YIELD_VALUE, (retval -- unused)) {
            // NOTE: It's important that YIELD_VALUE never raises an exception!
            // The compiler treats any exception raised here as a failed close()
            // or throw() call.
            assert(frame != &entry_frame);
            PyGenObject *gen = _PyFrame_GetGenerator(frame);
            gen->gi_frame_state = FRAME_SUSPENDED;
            _PyFrame_SetStackPointer(frame, stack_pointer - 1);
            tstate->exc_info = gen->gi_exc_state.previous_item;
            gen->gi_exc_state.previous_item = NULL;
            _Py_LeaveRecursiveCallPy(tstate);
            _PyInterpreterFrame *gen_frame = frame;
            frame = cframe.current_frame = frame->previous;
            gen_frame->previous = NULL;
            _PyFrame_StackPush(frame, retval);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        override inst(RETURN_GENERATOR, (--)) {
            assert(PyFunction_Check(frame->f_funcobj));
            PyFunctionObject *func = (PyFunctionObject *)frame->f_funcobj;
            PyGenObject *gen = (PyGenObject *)_Py_MakeCoro(func);
            if (gen == NULL) {
                goto error;
            }
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _PyInterpreterFrame *gen_frame = (_PyInterpreterFrame *)gen->gi_iframe;
            _PyFrame_Copy(frame, gen_frame);
            assert(frame->frame_obj == NULL);
            gen->gi_frame_state = FRAME_CREATED;
            gen_frame->owner = FRAME_OWNED_BY_GENERATOR;
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            _PyInterpreterFrame *prev = frame->previous;
            _PyThreadState_PopFrame(tstate, frame);
            frame = cframe.current_frame = prev;
            _PyFrame_StackPush(frame, (PyObject *)gen);

            CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

            goto resume_frame;
        }

        // END BYTECODES //
    }
}
