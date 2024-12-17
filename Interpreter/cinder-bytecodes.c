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

        override inst(NOP, (--)) {
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
                    Py_DECREF(tup);
                    goto error;
                }
                Py_SETREF(tup, PySequence_Tuple(tup));
                if (tup == NULL) {
                    goto error;
                }
            }
            element = PyTuple_GetItem(tup, idx);
            if (!element) {
                Py_DECREF(tup);
                goto error;
            }
            Py_INCREF(element);
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
      
        inst(CAST, (val -- val)) {
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
            Py_DECREF(type);
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
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
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
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
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
                DECREF_INPUTS();
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

            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

      // END BYTECODES //
    }
}
