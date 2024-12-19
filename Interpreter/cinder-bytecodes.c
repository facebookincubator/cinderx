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
            ERROR_IF(Ci_DictOrChecked_SetItem(dict, key, value) != 0, error);
            DECREF_INPUTS();
            PREDICT(JUMP_BACKWARD);
        }

        override inst(LIST_APPEND, (list, unused[oparg-1], v -- list, unused[oparg-1])) {
            ERROR_IF(Ci_ListOrCheckedList_Append((PyListObject*)list, v) < 0, error);
            Py_DECREF(v);
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
            ERROR_IF(!element, error);
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

        inst(TP_ALLOC, (-- inst)) {
            int optional;
            int exact;
            PyTypeObject* type = _PyClassLoader_ResolveType(
                GETITEM(frame->f_code->co_consts, oparg), &optional, &exact);
            assert(!optional);
            ERROR_IF(type == NULL, error);

            inst = type->tp_alloc(type, 0);
            ERROR_IF(inst == NULL, error);

#ifdef ADAPTIVE
            if (shadow.shadow != NULL) {
                int offset = _PyShadow_CacheCastType(&shadow, (PyObject*)type);
                if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, TP_ALLOC_CACHED, offset);
                }
            }
#endif
            Py_DECREF(type);
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

        inst(STORE_LOCAL, (val -- )) {
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

#ifdef ADAPTIVE
            if (shadow.shadow != NULL) {
                assert(type < 8);
                _PyShadow_PatchByteCode(
                    &shadow, next_instr, PRIMITIVE_STORE_FAST, (index << 4) | type);
            }
#endif
        }

        inst(LOAD_FIELD, (self -- value)) {
            PyObject* field = GETITEM(frame->f_code->co_consts, oparg);
            int field_type;
            Py_ssize_t offset =
                _PyClassLoader_ResolveFieldOffset(field, &field_type);
            if (offset == -1) {
                goto error;
            }

            if (field_type == TYPED_OBJECT) {
                value = *FIELD_OFFSET(self, offset);
#ifdef ADAPTIVE
                if (shadow.shadow != NULL) {
                    assert(offset % sizeof(PyObject*) == 0);
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        LOAD_OBJ_FIELD,
                        offset / sizeof(PyObject*));
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
#ifdef ADAPTIVE
                if (shadow.shadow != NULL) {
                    int pos = _PyShadow_CacheFieldType(&shadow, offset, field_type);
                    if (pos != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, LOAD_PRIMITIVE_FIELD, pos);
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

        inst(STORE_FIELD, (value, self --)) {
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
#ifdef ADAPTIVE
                if (shadow.shadow != NULL) {
                    assert(offset % sizeof(PyObject*) == 0);
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        STORE_OBJ_FIELD,
                        offset / sizeof(PyObject*));
                }
#endif
            } else {
#ifdef ADAPTIVE
                if (shadow.shadow != NULL) {
                    int pos = _PyShadow_CacheFieldType(&shadow, offset, field_type);
                    if (pos != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, STORE_PRIMITIVE_FIELD, pos);
                    }
                }
#endif
                store_field(field_type, (char*)addr, value);
            }
            Py_DECREF(self);
        }

        inst(CAST, (val -- res)) {
            int optional;
            int exact;
            PyTypeObject* type = _PyClassLoader_ResolveType(
                GETITEM(frame->f_code->co_consts, oparg), &optional, &exact);
            if (type == NULL) {
                goto error;
            }
            if (!_PyObject_TypeCheckOptional(val, type, optional, exact)) {
                CAST_COERCE_OR_ERROR(val, type, exact);
            }

#ifdef ADAPTIVE
            if (shadow.shadow != NULL) {
                int offset = _PyShadow_CacheCastType(&shadow, (PyObject*)type);
                if (offset != -1) {
                    if (optional) {
                    if (exact) {
                        _PyShadow_PatchByteCode(
                            &shadow, next_instr, CAST_CACHED_OPTIONAL_EXACT, offset);
                    } else {
                        _PyShadow_PatchByteCode(
                            &shadow, next_instr, CAST_CACHED_OPTIONAL, offset);
                    }
                    } else if (exact) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, CAST_CACHED_EXACT, offset);
                    } else {
                    _PyShadow_PatchByteCode(&shadow, next_instr, CAST_CACHED, offset);
                    }
                }
            }
#endif
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
                Py_INCREF(v);   // _Ci_StaticArray_Set steals the reference
                err = _Ci_StaticArray_Set(sequence, idx, v);

                if (err != 0) {
                    Py_DECREF(v);
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
            ERROR_IF(err != 0, error);
            DECREF_INPUTS();
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
            ERROR_IF(length == NULL, error);
            Py_DECREF(collection);
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
            switch (oparg) {
                INT_UNARY_OPCODE(PRIM_OP_NEG_INT, -)
                INT_UNARY_OPCODE(PRIM_OP_INV_INT, ~)
                DBL_UNARY_OPCODE(PRIM_OP_NEG_DBL, -)
                case PRIM_OP_NOT_INT: {
                    res = PyLong_AsVoidPtr(val) ? Py_False : Py_True;
                    Py_INCREF(res);
                }
                default:
                    PyErr_SetString(PyExc_RuntimeError, "unknown op");
                    goto error;
            }
            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
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
            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
        }

        inst(PRIMITIVE_BINARY_OP, (l, r -- res)) {
            switch (oparg) {
                INT_BIN_OPCODE_SIGNED(PRIM_OP_ADD_INT, +)
                INT_BIN_OPCODE_SIGNED(PRIM_OP_SUB_INT, -)
                INT_BIN_OPCODE_SIGNED(PRIM_OP_MUL_INT, *)
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
                INT_BIN_OPCODE_SIGNED(PRIM_OP_XOR_INT, ^)
                INT_BIN_OPCODE_SIGNED(PRIM_OP_OR_INT, |)
                INT_BIN_OPCODE_SIGNED(PRIM_OP_AND_INT, &)
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
                    goto error;
            }
            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
        }

        inst(PRIMITIVE_COMPARE_OP, (l, r -- res)) {
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
                    DECREF_INPUTS();
                    goto error;
            }
            DECREF_INPUTS();
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

        inst(INVOKE_FUNCTION, (args[invoke_function_args(frame->f_code->co_consts, oparg)] -- res)) {
            // We should move to encoding the number of args directly in the
            // opcode, right now pulling them out via invoke_function_args is a little
            // ugly.
            PyObject* value = GETITEM(frame->f_code->co_consts, oparg);
            int nargs = invoke_function_args(frame->f_code->co_consts, oparg);
            PyObject* target = PyTuple_GET_ITEM(value, 0);
            PyObject* container;
            PyObject* func = _PyClassLoader_ResolveFunction(target, &container);
            ERROR_IF(func == NULL, error);

            res = _PyObject_Vectorcall(func, args, nargs, NULL);
#ifdef ADAPTIVE
            if (shadow.shadow != NULL && nargs < 0x80) {
                if (_PyClassLoader_IsImmutable(container)) {
                    /* frozen type, we don't need to worry about indirecting */
                    int offset = _PyShadow_CacheCastType(&shadow, func);
                    if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        INVOKE_FUNCTION_CACHED,
                        (nargs << 8) | offset);
                    }
                } else {
                    PyObject** funcptr = _PyClassLoader_ResolveIndirectPtr(target);
                    int offset = _PyShadow_CacheFunction(&shadow, funcptr);
                    if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        INVOKE_FUNCTION_INDIRECT_CACHED,
                        (nargs << 8) | offset);
                    }
                }
            }
#endif
            Py_DECREF(func);
            Py_DECREF(container);
            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
        }

        inst(INVOKE_METHOD, (args[invoke_function_args(frame->f_code->co_consts, oparg) + 1] -- res)) {
            PyObject* value = GETITEM(frame->f_code->co_consts, oparg);
            Py_ssize_t nargs = invoke_function_args(frame->f_code->co_consts, oparg) + 1;
            PyObject* target = PyTuple_GET_ITEM(value, 0);
            int is_classmethod = PyTuple_GET_SIZE(value) == 3 &&
                (PyTuple_GET_ITEM(value, 2) == Py_True);

            Py_ssize_t slot = _PyClassLoader_ResolveMethod(target);
            if (slot == -1) {
                goto error;
            }

#ifdef ADAPTIVE
            assert(*(next_instr - 2) == EXTENDED_ARG);
            if (shadow.shadow != NULL && nargs < 0x80) {
                PyMethodDescrObject* method;
                if ((method = _PyClassLoader_ResolveMethodDef(target)) != NULL) {
                    int offset = _PyShadow_CacheCastType(&shadow, (PyObject*)method);
                    if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        INVOKE_FUNCTION_CACHED,
                        (nargs << 8) | offset);
                    }
                } else {
                    /* We smuggle in the information about whether the invocation was a
                    * classmethod in the low bit of the oparg. This is necessary, as
                    * without, the runtime won't be able to get the correct vtable from
                    * self when the type is passed in.
                    */
                    _PyShadow_PatchByteCode(
                        &shadow,
                        next_instr,
                        INVOKE_METHOD_CACHED,
                        (slot << 9) | (nargs << 1) | (is_classmethod ? 1 : 0));
                }
            }
#endif
            PyObject* self = *args;

            _PyType_VTable* vtable;
            if (is_classmethod) {
                vtable = (_PyType_VTable*)(((PyTypeObject*)self)->tp_cache);
            } else {
                vtable = (_PyType_VTable*)self->ob_type->tp_cache;
            }

            assert(!PyErr_Occurred());

            res = _PyClassLoader_InvokeMethod(
                vtable,
                slot,
                args,
                nargs);

            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
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
            ERROR_IF(res == NULL, error);
            DECREF_INPUTS();
        }

        inst(BUILD_CHECKED_LIST, (list_items[build_checked_obj_size(frame->f_code->co_consts, oparg)] -- list)) {
            PyObject* list_info = GETITEM(frame->f_code->co_consts, oparg);
            PyObject* list_type = PyTuple_GET_ITEM(list_info, 0);
            Py_ssize_t list_size = PyLong_AsLong(PyTuple_GET_ITEM(list_info, 1));

            int optional;
            int exact;
            PyTypeObject* type =
                _PyClassLoader_ResolveType(list_type, &optional, &exact);
            assert(!optional);

#ifdef ADAPTIVE
            if (shadow.shadow != NULL) {
                PyObject* cache = PyTuple_New(2);
                if (cache == NULL) {
                    goto error;
                }
                PyTuple_SET_ITEM(cache, 0, (PyObject*)type);
                Py_INCREF(type);
                PyObject* size = PyLong_FromLong(list_size);
                if (size == NULL) {
                    Py_DECREF(cache);
                    goto error;
                }
                PyTuple_SET_ITEM(cache, 1, size);

                int offset = _PyShadow_CacheCastType(&shadow, cache);
                Py_DECREF(cache);
                if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, BUILD_CHECKED_LIST_CACHED, offset);
                }
            }
#endif

            list = Ci_CheckedList_New(type, list_size);
            ERROR_IF(list == NULL, error);

            Py_DECREF(type);

            for (Py_ssize_t i = 0; i < list_size; i++) {
                Ci_ListOrCheckedList_SET_ITEM(list, i, list_items[i]);
            }
        }

        inst(BUILD_CHECKED_MAP, (map_items[build_checked_obj_size(frame->f_code->co_consts, oparg) * 2] -- map)) {
            PyObject* map_info = GETITEM(frame->f_code->co_consts, oparg);
            PyObject* map_type = PyTuple_GET_ITEM(map_info, 0);
            Py_ssize_t map_size = PyLong_AsLong(PyTuple_GET_ITEM(map_info, 1));

            int optional;
            int exact;
            PyTypeObject* type =
                _PyClassLoader_ResolveType(map_type, &optional, &exact);
            assert(!optional);

#ifdef ADAPTIVE
            if (shadow.shadow != NULL) {
                PyObject* cache = PyTuple_New(2);
                if (cache == NULL) {
                    goto error;
                }
                PyTuple_SET_ITEM(cache, 0, (PyObject*)type);
                Py_INCREF(type);
                PyObject* size = PyLong_FromLong(map_size);
                if (size == NULL) {
                    Py_DECREF(cache);
                    goto error;
                }
                PyTuple_SET_ITEM(cache, 1, size);

                int offset = _PyShadow_CacheCastType(&shadow, cache);
                Py_DECREF(cache);
                if (offset != -1) {
                    _PyShadow_PatchByteCode(
                        &shadow, next_instr, BUILD_CHECKED_MAP_CACHED, offset);
                }
            }
#endif

            map = Ci_CheckedDict_NewPresized(type, map_size);
            if (map == NULL) {
                goto error;
            }
            Py_DECREF(type);

            Ci_BUILD_DICT(map_size, Ci_CheckedDict_SetItem);
            ERROR_IF(map == NULL, error);
            DECREF_INPUTS();
        }
      // END BYTECODES //
    }
}
