// This file contains instruction definitions.
// It is read by generators stored in Tools/cases_generator/
// to generate Python/generated_cases.c.h and others.
// Note that there is some dummy C code at the top and bottom of the file
// to fool text editors like VS Code into believing this is valid C code.
// The actual instruction definitions start at // BEGIN BYTECODES //.
// See Tools/cases_generator/README.md for more information.

#include "Python.h"
#include "dictobject.h"
#include "opcode.h"
#include "optimizer.h"
#include "pycore_abstract.h" // _PyIndex_Check()
#include "pycore_audit.h" // _PySys_Audit()
#include "pycore_backoff.h"
#include "pycore_cell.h" // PyCell_GetRef()
#include "pycore_code.h"
#include "pycore_dict.h"
#include "pycore_emscripten_signal.h" // _Py_CHECK_EMSCRIPTEN_SIGNALS
#include "pycore_frame.h"
#include "pycore_function.h"
#include "pycore_instruments.h"
#include "pycore_interpolation.h" // _PyInterpolation_Build()
#include "pycore_intrinsics.h"
#include "pycore_long.h" // _PyLong_ExactDealloc(), _PyLong_GetZero()
#include "pycore_moduleobject.h" // PyModuleObject
#include "pycore_object.h" // _PyObject_GC_TRACK()
#include "pycore_opcode_metadata.h" // uop names
#include "pycore_opcode_utils.h" // MAKE_FUNCTION_*
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_*
#include "pycore_pyerrors.h" // _PyErr_GetRaisedException()
#include "pycore_pystate.h" // _PyInterpreterState_GET()
#include "pycore_range.h" // _PyRangeIterObject
#include "pycore_setobject.h" // _PySet_NextEntry()
#include "pycore_sliceobject.h" // _PyBuildSlice_ConsumeRefs
#include "pycore_stackref.h"
#include "pycore_template.h" // _PyTemplate_Build()
#include "pycore_tuple.h" // _PyTuple_ITEMS()
#include "pycore_typeobject.h" // _PySuper_Lookup()
#include "pydtrace.h"
#include "setobject.h"

#define USE_COMPUTED_GOTOS 0
#include "Python/ceval_macros.h"

/* Flow control macros */

#define inst(name, ...) case name:
#define op(name, ...) /* NAME is ignored */
#define macro(name) static int MACRO_##name
#define super(name) static int SUPER_##name
#define family(name, ...) static int family_##name
#define pseudo(name) static int pseudo_##name
#define label(name) \
  name:

/* Annotations */
#define guard
#define override
#define specializing
#define replicate(TIMES)
#define tier1
#define no_save_ip

// Dummy variables for stack effects.
static PyObject *value, *value1, *value2, *left, *right, *res, *sum, *prod,
    *sub;
static PyObject *container, *start, *stop, *v, *lhs, *rhs, *res2;
static PyObject *list, *tuple, *dict, *owner, *set, *str, *tup, *map, *keys;
static PyObject *exit_func, *lasti, *val, *retval, *obj, *iter, *exhausted;
static PyObject *aiter, *awaitable, *iterable, *w, *exc_value, *bc, *locals;
static PyObject *orig, *excs, *update, *b, *fromlist, *level, *from;
static PyObject **pieces, **values;
static size_t jump;
// Dummy variables for cache effects
static uint16_t invert, counter, index, hint;
#define unused 0 // Used in a macro def, can't be static
static uint32_t type_version;
static _PyExecutorObject* current_executor;

static PyObject* dummy_func(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    unsigned char opcode,
    unsigned int oparg,
    _Py_CODEUNIT* next_instr,
    PyObject** stack_pointer,
    int throwflag,
    PyObject* args[]) {
// Dummy labels.
pop_1_error:
  // Dummy locals.
  PyObject* dummy;
  _Py_CODEUNIT* this_instr;
  PyObject* attr;
  PyObject* attrs;
  PyObject* bottom;
  PyObject* callable;
  PyObject* callargs;
  PyObject* codeobj;
  PyObject* cond;
  PyObject* descr;
  PyObject* exc;
  PyObject* exit;
  PyObject* fget;
  PyObject* fmt_spec;
  PyObject* func;
  uint32_t func_version;
  PyObject* getattribute;
  PyObject* kwargs;
  PyObject* kwdefaults;
  PyObject* len_o;
  PyObject* match;
  PyObject* match_type;
  PyObject* method;
  PyObject* mgr;
  Py_ssize_t min_args;
  PyObject* names;
  PyObject* new_exc;
  PyObject* next;
  PyObject* none;
  PyObject* null;
  PyObject* prev_exc;
  PyObject* receiver;
  PyObject* rest;
  int result;
  PyObject* self;
  PyObject* seq;
  PyObject* slice;
  PyObject* step;
  PyObject* subject;
  PyObject* top;
  PyObject* type;
  PyObject* typevars;
  PyObject* val0;
  PyObject* val1;
  int values_or_none;

  switch (opcode) {
    // BEGIN BYTECODES //
    override op(_PUSH_FRAME, (new_frame--)) {
      // Write it out explicitly because it's subtly different.
      // Eventually this should be the only occurrence of this code.
      assert(!IS_PEP523_HOOKED(tstate));
      _PyInterpreterFrame *temp = PyStackRef_Unwrap(new_frame);
      DEAD(new_frame);
      SYNC_SP();
      _PyFrame_SetStackPointer(frame, stack_pointer);
      assert(temp->previous == frame || temp->previous->previous == frame);
      CALL_STAT_INC(inlined_py_calls);
      frame = tstate->current_frame = temp;
      tstate->py_recursion_remaining--;
      LOAD_SP();
      LOAD_IP(0);

      CI_UPDATE_CALL_COUNT

      LLTRACE_RESUME_FRAME();
    }

    override inst(
        MAP_ADD,
        (dict_st, unused[oparg - 1], key, value-- dict_st, unused[oparg - 1])) {
      PyObject* dict = PyStackRef_AsPyObjectBorrow(dict_st);
      /* dict[key] = value */
      int err = Ci_DictOrChecked_SetItem(
          dict,
          PyStackRef_AsPyObjectBorrow(key),
          PyStackRef_AsPyObjectBorrow(value));
      PyStackRef_CLOSE(value);
      PyStackRef_CLOSE(key);
      ERROR_IF(err != 0);
    }

    override inst(
        LIST_APPEND, (list, unused[oparg - 1], v-- list, unused[oparg - 1])) {
      int err = Ci_ListOrCheckedList_Append(
          (PyListObject*)PyStackRef_AsPyObjectBorrow(list),
          PyStackRef_AsPyObjectBorrow(v));
      PyStackRef_CLOSE(v);
      ERROR_IF(err < 0);
    }

    override inst(EXTENDED_OPCODE, (args[oparg >> 2]-- top[oparg & 0x03])) {
      // Decode any extended oparg
      int extop = (int)next_instr->op.code;
      int extoparg = (int)next_instr->op.arg;
      while (extop == EXTENDED_ARG) {
        SKIP_OVER(1);
        extoparg = extoparg << 8 | next_instr->op.arg;
        extop = next_instr->op.code;
      }
      extop |= EXTENDED_OPCODE_FLAG;

      // Switch isn't supported in opcodes
      if (extop == PRIMITIVE_LOAD_CONST) {
        top[0] = PyStackRef_FromPyObjectNew(
            PyTuple_GET_ITEM(GETITEM(FRAME_CO_CONSTS, extoparg), 0));
        DECREF_INPUTS();
      } else if (extop == STORE_LOCAL) {
        _PyStackRef val = args[0];
        PyObject* local = GETITEM(FRAME_CO_CONSTS, extoparg);
        int index = PyLong_AsInt(PyTuple_GET_ITEM(local, 0));
        int type =
            _PyClassLoader_ResolvePrimitiveType(PyTuple_GET_ITEM(local, 1));

        if (type < 0) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        _PyStackRef tmp = GETLOCAL(index);
        if (type == TYPED_DOUBLE) {
          GETLOCAL(index) = PyStackRef_DUP(val);
        } else {
          Py_ssize_t ival =
              unbox_primitive_int(PyStackRef_AsPyObjectBorrow(val));
          GETLOCAL(index) =
              PyStackRef_FromPyObjectSteal(box_primitive(type, ival));
        }

        PyStackRef_XCLOSE(tmp);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          if (index < INT8_MAX && type < INT8_MAX) {
            int16_t* cache = (int16_t*)next_instr;
            *cache = (index << 8) | type;
            _Ci_specialize(next_instr, STORE_LOCAL_CACHED);
          }
        }
#endif
        DECREF_INPUTS();
      } else if (extop == LOAD_LOCAL) {
        int index = PyLong_AsInt(
            PyTuple_GET_ITEM(GETITEM(FRAME_CO_CONSTS, extoparg), 0));

        _PyStackRef value = GETLOCAL(index);
        if (PyStackRef_IsNull(value)) {
          // Primitive values are default initialized to zero, so they don't
          // need to be defined. We should consider stop doing that as it can
          // cause compatibility issues when the same code runs statically and
          // non statically.
          GETLOCAL(index) = value =
              PyStackRef_FromPyObjectSteal(PyLong_FromLong(0));
        }
        value = PyStackRef_DUP(value);
        DECREF_INPUTS();
        top[0] = value;
      } else if (extop == PRIMITIVE_BOX) {
        top[0] = sign_extend_primitive(args[0], extoparg);
        DEAD(args);
      } else if (extop == PRIMITIVE_UNBOX) {
        PyObject* val = PyStackRef_AsPyObjectBorrow(args[0]);
        if (PyLong_CheckExact(val)) {
          size_t value;
          int overflow = _PyClassLoader_CheckOverflow(val, extoparg, &value);
          if (!overflow) {
            PyErr_SetString(PyExc_OverflowError, "int overflow");
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        }
        DEAD(args);
      } else if (extop == SEQUENCE_GET) {
        PyObject* sequence = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* idx = PyStackRef_AsPyObjectBorrow(args[1]);
        PyObject* item;
        Py_ssize_t val = (Py_ssize_t)PyLong_AsVoidPtr(idx);

        if (val == -1 && _PyErr_Occurred(tstate)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        // Adjust index
        if (val < 0) {
          val += Py_SIZE(sequence);
        }

        extoparg &= ~SEQ_SUBSCR_UNCHECKED;

        if (extoparg == SEQ_LIST) {
          item = PyList_GetItem(sequence, val);
          if (item == NULL) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
          Py_INCREF(item);
        } else if (extoparg == SEQ_LIST_INEXACT) {
          if (PyList_CheckExact(sequence) ||
              Py_TYPE(sequence)->tp_as_sequence->sq_item ==
                  PyList_Type.tp_as_sequence->sq_item) {
            item = PyList_GetItem(sequence, val);
            if (item == NULL) {
              DECREF_INPUTS();
              ERROR_IF(true);
            }
            Py_INCREF(item);
          } else {
            item = PyObject_GetItem(sequence, idx);
            if (item == NULL) {
              DECREF_INPUTS();
              ERROR_IF(true);
            }
          }
        } else if (extoparg == SEQ_CHECKED_LIST) {
          item = Ci_CheckedList_GetItem(sequence, val);
          if (item == NULL) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        } else if (extoparg == SEQ_ARRAY_INT64) {
          item = _Ci_StaticArray_Get(sequence, val);
          if (item == NULL) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        } else {
          PyErr_Format(
              PyExc_SystemError, "bad oparg for SEQUENCE_GET: %d", extoparg);
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(item);
      } else if (extop == SEQUENCE_SET) {
        PyObject* v = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* sequence = PyStackRef_AsPyObjectBorrow(args[1]);
        PyObject* subscr = PyStackRef_AsPyObjectBorrow(args[2]);
        int err;

        Py_ssize_t idx = (Py_ssize_t)PyLong_AsVoidPtr(subscr);

        if (idx == -1 && _PyErr_Occurred(tstate)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        // Adjust index
        if (idx < 0) {
          idx += Py_SIZE(sequence);
        }

        if (extoparg == SEQ_LIST) {
          Py_INCREF(v); // PyList_SetItem steals the reference
          err = PyList_SetItem(sequence, idx, v);

          if (err != 0) {
            Py_DECREF(v);
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        } else if (extoparg == SEQ_LIST_INEXACT) {
          if (PyList_CheckExact(sequence) ||
              Py_TYPE(sequence)->tp_as_sequence->sq_ass_item ==
                  PyList_Type.tp_as_sequence->sq_ass_item) {
            Py_INCREF(v); // PyList_SetItem steals the reference
            err = PyList_SetItem(sequence, idx, v);

            if (err != 0) {
              Py_DECREF(v);
              DECREF_INPUTS();
              ERROR_IF(true);
            }
          } else {
            err = PyObject_SetItem(sequence, subscr, v);
            if (err != 0) {
              DECREF_INPUTS();
              ERROR_IF(true);
            }
          }
        } else if (extoparg == SEQ_ARRAY_INT64) {
          err = _Ci_StaticArray_Set(sequence, idx, v);

          if (err != 0) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        } else {
          PyErr_Format(
              PyExc_SystemError, "bad oparg for SEQUENCE_SET: %d", oparg);
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        DECREF_INPUTS();
      } else if (extop == FAST_LEN) {
        PyObject* collection = PyStackRef_AsPyObjectBorrow(args[0]);
        int inexact = extoparg & FAST_LEN_INEXACT;
        extoparg &= ~FAST_LEN_INEXACT;
        assert(FAST_LEN_LIST <= extoparg && extoparg <= FAST_LEN_STR);
        PyObject* length;
        if (inexact) {
          // see if we have an exact type match, and if so, use the fastpath.
          if ((extoparg == FAST_LEN_LIST && PyList_CheckExact(collection)) ||
              (extoparg == FAST_LEN_DICT && PyDict_CheckExact(collection)) ||
              (extoparg == FAST_LEN_SET && PyAnySet_CheckExact(collection)) ||
              (extoparg == FAST_LEN_TUPLE && PyTuple_CheckExact(collection)) ||
              (extoparg == FAST_LEN_ARRAY &&
               PyStaticArray_CheckExact(collection)) ||
              (extoparg == FAST_LEN_STR && PyUnicode_CheckExact(collection))) {
            inexact = 0;
          }
        }
        if (inexact) {
          Py_ssize_t res = PyObject_Size(collection);
          length = res >= 0 ? PyLong_FromSsize_t(res) : NULL;
        } else if (extoparg == FAST_LEN_DICT) {
          if (Ci_CheckedDict_Check(collection)) {
            length = PyLong_FromLong(PyObject_Size(collection));
          } else {
            assert(PyDict_Check(collection));
            length = PyLong_FromLong(((PyDictObject*)collection)->ma_used);
          }
        } else if (extoparg == FAST_LEN_SET) {
          assert(PyAnySet_Check(collection));
          length = PyLong_FromLong(((PySetObject*)collection)->used);
        } else {
          // lists, tuples, arrays are all PyVarObject and use ob_size
          assert(
              PyTuple_Check(collection) || PyList_Check(collection) ||
              PyStaticArray_CheckExact(collection) ||
              PyUnicode_Check(collection) || Ci_CheckedList_Check(collection));
          length = PyLong_FromLong(Py_SIZE(collection));
        }
        DECREF_INPUTS();
        ERROR_IF(length == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(length);
      } else if (extop == LIST_DEL) {
        PyObject* list = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* subscr = PyStackRef_AsPyObjectBorrow(args[1]);
        int err;

        Py_ssize_t idx = PyLong_AsLong(subscr);

        if (idx == -1 && _PyErr_Occurred(tstate)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        err = PyList_SetSlice(list, idx, idx + 1, NULL);
        DECREF_INPUTS();
        ERROR_IF(err != 0);
      } else if (extop == REFINE_TYPE) {
        DEAD(args);
      } else if (extop == LOAD_CLASS) {
        PyObject* type_descr = GETITEM(FRAME_CO_CONSTS, extoparg);
        int optional;
        int exact;
        PyObject* type = (PyObject*)_PyClassLoader_ResolveType(
            type_descr, &optional, &exact);
        DECREF_INPUTS();
        ERROR_IF(type == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(type);
      } else if (extop == LOAD_TYPE) {
        PyObject* instance = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* type = (PyObject*)Py_TYPE(instance);
        Py_INCREF(type);
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(type);
      } else if (extop == BUILD_CHECKED_LIST) {
        PyObject* list;
        PyObject* list_info = GETITEM(FRAME_CO_CONSTS, extoparg);
        PyObject* list_type = PyTuple_GET_ITEM(list_info, 0);
        Py_ssize_t list_size = PyLong_AsLong(PyTuple_GET_ITEM(list_info, 1));

        int optional;
        int exact;
        PyTypeObject* type =
            _PyClassLoader_ResolveType(list_type, &optional, &exact);
        assert(!optional);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          specialize_with_value(
              next_instr, (PyObject*)type, BUILD_CHECKED_LIST_CACHED, 0, 0);
        }
#endif

        list = Ci_CheckedList_New(type, list_size);
        Py_DECREF(type);

        if (list == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        for (Py_ssize_t i = 0; i < list_size; i++) {
          Ci_ListOrCheckedList_SET_ITEM(
              list, i, PyStackRef_AsPyObjectBorrow(args[i]));
        }
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(list);
      } else if (extop == BUILD_CHECKED_MAP) {
        PyObject* map_info = GETITEM(FRAME_CO_CONSTS, extoparg);
        PyObject* map_type = PyTuple_GET_ITEM(map_info, 0);
        Py_ssize_t map_size = PyLong_AsLong(PyTuple_GET_ITEM(map_info, 1));

        int optional;
        int exact;
        PyTypeObject* type =
            _PyClassLoader_ResolveType(map_type, &optional, &exact);
        if (type == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }
        assert(!optional);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          specialize_with_value(
              next_instr, (PyObject*)type, BUILD_CHECKED_MAP_CACHED, 0, 0);
        }
#endif

        PyObject* map = Ci_CheckedDict_NewPresized(type, map_size);
        Py_DECREF(type);
        if (map == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        if (ci_build_dict(args, map_size, map) < 0) {
          Py_DECREF(map);
          DECREF_INPUTS();
          ERROR_IF(true);
        }
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(map);
      } else if (extop == LOAD_METHOD_STATIC) {
        PyObject* self = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* value = GETITEM(FRAME_CO_CONSTS, extoparg);
        PyObject* target = PyTuple_GET_ITEM(value, 0);
        int is_classmethod = _PyClassLoader_IsClassMethodDescr(value);

        Py_ssize_t slot = _PyClassLoader_ResolveMethod(target);
        if (slot == -1) {
          DECREF_INPUTS();
          ERROR_IF(true);
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
            int32_t* cache = (int32_t*)next_instr;
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
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        _PyStackRef self_ref = PyStackRef_DUP(args[0]);
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(res.lmr_func);
        top[1] = self_ref;
      } else if (extop == INVOKE_METHOD) {
        PyObject* target = PyStackRef_AsPyObjectBorrow(args[0]);
        Py_ssize_t nargs = (oparg >> 2) - 1;

        assert(!PyErr_Occurred());

        STACKREFS_TO_PYOBJECTS(&args[1], nargs, args_o);
        if (CONVERSION_FAILED(args_o)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }
        PyObject* res = PyObject_Vectorcall(target, args_o, nargs, NULL);
        STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == INVOKE_FUNCTION) {
        // We should move to encoding the number of args directly in the
        // opcode, right now pulling them out via invoke_function_args is a
        // little ugly.
        PyObject* value = GETITEM(FRAME_CO_CONSTS, extoparg);
        int nargs = oparg >> 2;
        PyObject* target = PyTuple_GET_ITEM(value, 0);
        PyObject* container;
        PyObject* func = _PyClassLoader_ResolveFunction(target, &container);
        if (func == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        STACKREFS_TO_PYOBJECTS(args, nargs, args_o);
        if (CONVERSION_FAILED(args_o)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }
        PyObject* res = _PyObject_Vectorcall(func, args_o, nargs, NULL);
        STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          if (_PyClassLoader_IsImmutable(container)) {
            /* frozen type, we don't need to worry about indirecting */
            specialize_with_value(
                next_instr, func, INVOKE_FUNCTION_CACHED, 0, 0);
          } else {
            PyObject** funcptr = _PyClassLoader_ResolveIndirectPtr(target);
            PyObject*** cache = (PyObject***)next_instr;
            *cache = funcptr;
            _Ci_specialize(next_instr, INVOKE_INDIRECT_CACHED);
          }
        }
#endif
        Py_DECREF(func);
        Py_DECREF(container);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == INVOKE_NATIVE) {
        PyObject* value = GETITEM(FRAME_CO_CONSTS, extoparg);
        assert(PyTuple_CheckExact(value));
        Py_ssize_t nargs = oparg >> 2;

        PyObject* target = PyTuple_GET_ITEM(value, 0);
        PyObject* name = PyTuple_GET_ITEM(target, 0);
        PyObject* symbol = PyTuple_GET_ITEM(target, 1);
        PyObject* signature = PyTuple_GET_ITEM(value, 1);

        STACKREFS_TO_PYOBJECTS(args, nargs, args_o);
        if (CONVERSION_FAILED(args_o)) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        PyObject* res = _PyClassloader_InvokeNativeFunction(
            name, symbol, signature, args_o, nargs);
        STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == TP_ALLOC) {
        int optional;
        int exact;
        PyTypeObject* type = _PyClassLoader_ResolveType(
            GETITEM(FRAME_CO_CONSTS, extoparg), &optional, &exact);
        assert(!optional);
        if (type == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        PyObject* inst = type->tp_alloc(type, 0);

#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          specialize_with_value(next_instr, func, TP_ALLOC_CACHED, 0, 0);
        }
#endif
        Py_DECREF(type);
        DECREF_INPUTS();
        ERROR_IF(inst == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(inst);
      } else if (extop == CAST) {
        PyObject* val = PyStackRef_AsPyObjectBorrow(args[0]);
        int optional;
        int exact;
        PyTypeObject* type = _PyClassLoader_ResolveType(
            GETITEM(FRAME_CO_CONSTS, extoparg), &optional, &exact);
        if (type == NULL) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
        if (adaptive_enabled) {
          specialize_with_value(
              next_instr,
              (PyObject*)type,
              CAST_CACHED,
              2,
              (exact << 1) | optional);
        }
#endif
        _PyStackRef res;
        if (!_PyObject_TypeCheckOptional(val, type, optional, exact)) {
          if (type == &PyFloat_Type && PyObject_TypeCheck(val, &PyLong_Type)) {
            double dval = PyLong_AsDouble(val);
            if (dval == -1.0 && PyErr_Occurred()) {
              DECREF_INPUTS();
              ERROR_IF(true);
            }
            PyObject* fval = PyFloat_FromDouble(dval);
            if (fval == NULL) {
              DECREF_INPUTS();
              ERROR_IF(true);
            }
            res = PyStackRef_FromPyObjectSteal(fval);
          } else {
            PyErr_Format(
                PyExc_TypeError,
                exact ? "expected exactly '%s', got '%s'"
                      : "expected '%s', got '%s'",
                type->tp_name,
                Py_TYPE(val)->tp_name);
            Py_DECREF(type);
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        } else {
          res = PyStackRef_FromPyObjectNew(val);
        }

        Py_DECREF(type);
        DECREF_INPUTS();
        top[0] = res;
      } else if (extop == PRIMITIVE_UNARY_OP) {
        PyObject* res =
            primitive_unary_op(PyStackRef_AsPyObjectBorrow(args[0]), extoparg);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == PRIMITIVE_BINARY_OP) {
        PyObject* res = primitive_binary_op(
            PyStackRef_AsPyObjectBorrow(args[0]),
            PyStackRef_AsPyObjectBorrow(args[1]),
            extoparg);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == PRIMITIVE_COMPARE_OP) {
        PyObject* res = primitive_compare_op(
            PyStackRef_AsPyObjectBorrow(args[0]),
            PyStackRef_AsPyObjectBorrow(args[1]),
            extoparg);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == LOAD_FIELD) {
        PyObject* self = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* field = GETITEM(FRAME_CO_CONSTS, extoparg);
        PyObject* value;
        int field_type;
        Py_ssize_t offset =
            _PyClassLoader_ResolveFieldOffset(field, &field_type);
        if (offset == -1) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        if (field_type == TYPED_OBJECT) {
          value = *FIELD_OFFSET(self, offset);
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
          if (adaptive_enabled) {
            if (offset < INT32_MAX) {
              int32_t* cache = (int32_t*)next_instr;
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
            DECREF_INPUTS();
            ERROR_IF(true);
          }
          Py_INCREF(value);
        } else {
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
          if (adaptive_enabled) {
            if (offset <= INT32_MAX >> 8) {
              assert(field_type < 0xff);
              int32_t* cache = (int32_t*)next_instr;
              *cache = offset << 8 | field_type;
              _Ci_specialize(next_instr, LOAD_PRIMITIVE_FIELD);
            }
          }
#endif

          value = load_field(field_type, (char*)FIELD_OFFSET(self, offset));
          if (value == NULL) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
        }
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(value);
      } else if (extop == STORE_FIELD) {
        PyObject* value = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* self = PyStackRef_AsPyObjectBorrow(args[1]);
        PyObject* field = GETITEM(FRAME_CO_CONSTS, extoparg);
        int field_type;
        Py_ssize_t offset =
            _PyClassLoader_ResolveFieldOffset(field, &field_type);
        if (offset == -1) {
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        PyObject** addr = FIELD_OFFSET(self, offset);

        if (field_type == TYPED_OBJECT) {
          Py_INCREF(value);
          Py_XDECREF(*addr);
          *addr = value;
#if ENABLE_SPECIALIZATION && defined(ENABLE_ADAPTIVE_STATIC_PYTHON)
          if (adaptive_enabled) {
            if (offset <= INT32_MAX) {
              int32_t* cache = (int32_t*)next_instr;
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
              int32_t* cache = (int32_t*)next_instr;
              *cache = offset << 8 | field_type;
              _Ci_specialize(next_instr, STORE_PRIMITIVE_FIELD);
            }
          }
#endif
          store_field(field_type, (char*)addr, value);
        }
        DECREF_INPUTS();
      } else if (extop == RETURN_PRIMITIVE) {
        assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
        _PyStackRef temp =
            sign_extend_primitive(PyStackRef_MakeHeapSafe(args[0]), extoparg);
        DEAD(args);
        SAVE_STACK();
        assert(STACK_LEVEL() == 0);
        _Py_LeaveRecursiveCallPy(tstate);
        // GH-99729: We need to unlink the frame *before* clearing it:
        _PyInterpreterFrame* dying = frame;
        frame = tstate->current_frame = dying->previous;
        _PyEval_FrameClearAndPop(tstate, dying);
        RELOAD_STACK();
        LOAD_IP(frame->return_offset);
        stack_pointer[0] = temp;
        stack_pointer += 1;
        LLTRACE_RESUME_FRAME();
        DISPATCH();
      } else if (extop == POP_JUMP_IF_ZERO) {
        PyObject* cond = PyStackRef_AsPyObjectBorrow(args[0]);
        int is_nonzero = PyObject_IsTrue(cond);
        DECREF_INPUTS();
        SKIP_OVER(2); // skip cache + EXTENDED_OPCODE
        if (!is_nonzero) {
          JUMPBY(extoparg);
          DISPATCH();
        }
        DISPATCH();
      } else if (extop == POP_JUMP_IF_NONZERO) {
        PyObject* cond = PyStackRef_AsPyObjectBorrow(args[0]);
        int is_nonzero = PyObject_IsTrue(cond);
        DECREF_INPUTS();
        SKIP_OVER(2); // skip cache and EXTENDED_OPCODE
        if (is_nonzero) {
          JUMPBY(extoparg);
          DISPATCH();
        }
        DISPATCH();
      } else if (extop == CONVERT_PRIMITIVE) {
        PyObject* val = PyStackRef_AsPyObjectBorrow(args[0]);
        Py_ssize_t from_type = extoparg & 0xFF;
        Py_ssize_t to_type = extoparg >> 4;
        Py_ssize_t extend_sign =
            (from_type & TYPED_INT_SIGNED) && (to_type & TYPED_INT_SIGNED);
        int size = to_type >> 1;
        size_t ival = (size_t)PyLong_AsVoidPtr(val);

        ival &= trunc_masks[size];

        // Extend the sign if needed
        if (extend_sign != 0 && (ival & signed_bits[size])) {
          ival |= (signex_masks[size]);
        }

        PyObject* res = PyLong_FromSize_t(ival);
        DECREF_INPUTS();
        ERROR_IF(res == NULL);
        top[0] = PyStackRef_FromPyObjectSteal(res);
      } else if (extop == LOAD_ITERABLE_ARG) {
        PyObject* tup = PyStackRef_AsPyObjectBorrow(args[0]);
        PyObject* element;
        int idx = extoparg;
        _PyStackRef new_tup;
        if (!PyTuple_CheckExact(tup)) {
          if (tup->ob_type->tp_iter == NULL && !PySequence_Check(tup)) {
            PyErr_Format(
                PyExc_TypeError,
                "argument after * "
                "must be an iterable, not %.200s",
                tup->ob_type->tp_name);
            DECREF_INPUTS();
            ERROR_IF(true);
          }
          tup = PySequence_Tuple(tup);
          if (tup == NULL) {
            DECREF_INPUTS();
            ERROR_IF(true);
          }
          new_tup = PyStackRef_FromPyObjectSteal(tup);
        } else {
          new_tup = PyStackRef_FromPyObjectNew(tup);
        }

        element = PyTuple_GetItem(tup, idx);
        if (element == NULL) {
          PyStackRef_CLOSE(new_tup);
          DECREF_INPUTS();
          ERROR_IF(true);
        }
        Py_INCREF(element);
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(element);
        top[1] = new_tup;
      } else if (extop == LOAD_MAPPING_ARG) {
        PyObject *defaultval, *mapping, *name;
        if (extoparg == 3) {
          defaultval = PyStackRef_AsPyObjectBorrow(args[0]);
          mapping = PyStackRef_AsPyObjectBorrow(args[1]);
          name = PyStackRef_AsPyObjectBorrow(args[2]);
        } else {
          defaultval = NULL;
          mapping = PyStackRef_AsPyObjectBorrow(args[0]);
          name = PyStackRef_AsPyObjectBorrow(args[1]);
        }
        PyObject* value;
        if (!PyDict_Check(mapping) && !Ci_CheckedDict_Check(mapping)) {
          PyErr_Format(
              PyExc_TypeError,
              "argument after ** "
              "must be a dict, not %.200s",
              mapping->ob_type->tp_name);
          DECREF_INPUTS();
          ERROR_IF(true);
        }

        value = PyDict_GetItemWithError(mapping, name);
        if (value == NULL) {
          if (_PyErr_Occurred(tstate)) {
            DECREF_INPUTS();
            ERROR_IF(true);
          } else if (oparg == 2) {
            PyErr_Format(PyExc_TypeError, "missing argument %U", name);
            assert(defaultval == NULL);
            DECREF_INPUTS();
            ERROR_IF(true);
          } else {
            /* Default value is on the stack */
            value = defaultval;
          }
        }

        Py_INCREF(value);
        DECREF_INPUTS();
        top[0] = PyStackRef_FromPyObjectSteal(value);
      } else {
        PyErr_Format(
            PyExc_RuntimeError, "unsupported extended opcode: %d", extop);
        DECREF_INPUTS();
        ERROR_IF(true);
      }
      SKIP_OVER(1);
    }

    override inst(RETURN_VALUE, (retval-- res)) {
      assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
      _PyStackRef temp = PyStackRef_MakeHeapSafe(retval);
      DEAD(retval);
      SAVE_STACK();
      assert(STACK_LEVEL() == 0);
      _Py_LeaveRecursiveCallPy(tstate);
      // GH-99729: We need to unlink the frame *before* clearing it:
      _PyInterpreterFrame* dying = frame;
      frame = tstate->current_frame = dying->previous;

      // CX: Maybe reactivate adaptive interpreter in caller
      CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

      _PyEval_FrameClearAndPop(tstate, dying);
      RELOAD_STACK();
      LOAD_IP(frame->return_offset);
      res = temp;
      LLTRACE_RESUME_FRAME();
    }

    override inst(RETURN_GENERATOR, (--res)) {
      assert(PyStackRef_FunctionCheck(frame->f_funcobj));
      PyFunctionObject* func =
          (PyFunctionObject*)PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
      PyGenObject* gen = (PyGenObject*)_Py_MakeCoro(func);
      ERROR_IF(gen == NULL);
      assert(STACK_LEVEL() <= 2);
      SAVE_STACK();
      _PyInterpreterFrame* gen_frame = &gen->gi_iframe;
      frame->instr_ptr++;
      _PyFrame_Copy(frame, gen_frame);
      assert(frame->frame_obj == NULL);
      gen->gi_frame_state = FRAME_CREATED;
      gen_frame->owner = FRAME_OWNED_BY_GENERATOR;
      _Py_LeaveRecursiveCallPy(tstate);
      _PyInterpreterFrame* prev = frame->previous;
      _PyThreadState_PopFrame(tstate, frame);
      frame = tstate->current_frame = prev;

      // CX: Maybe reactivate adaptive interpreter in caller
      CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

      LOAD_IP(frame->return_offset);
      RELOAD_STACK();
      res = PyStackRef_FromPyObjectStealMortal((PyObject*)gen);
      LLTRACE_RESUME_FRAME();
    }

    override inst(YIELD_VALUE, (retval-- value)) {
      // NOTE: It's important that YIELD_VALUE never raises an exception!
      // The compiler treats any exception raised here as a failed close()
      // or throw() call.
      assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
      frame->instr_ptr++;
      PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
      assert(FRAME_SUSPENDED_YIELD_FROM == FRAME_SUSPENDED + 1);
      assert(oparg == 0 || oparg == 1);
      _PyStackRef temp = retval;
      DEAD(retval);
      SAVE_STACK();
      tstate->exc_info = gen->gi_exc_state.previous_item;
      gen->gi_exc_state.previous_item = NULL;
      _Py_LeaveRecursiveCallPy(tstate);
      _PyInterpreterFrame* gen_frame = frame;
      frame = tstate->current_frame = frame->previous;

      // CX: Maybe reactivate adaptive interpreter in caller
      CI_SET_ADAPTIVE_INTERPRETER_ENABLED_STATE

      gen_frame->previous = NULL;
      ((_PyThreadStateImpl *)tstate)->generator_return_kind = GENERATOR_YIELD;
      FT_ATOMIC_STORE_INT8_RELEASE(gen->gi_frame_state, FRAME_SUSPENDED + oparg);
      /* We don't know which of these is relevant here, so keep them equal */
      assert(INLINE_CACHE_ENTRIES_SEND == INLINE_CACHE_ENTRIES_FOR_ITER);
#if TIER_ONE
      assert(
          frame->instr_ptr->op.code == INSTRUMENTED_LINE ||
          frame->instr_ptr->op.code == INSTRUMENTED_INSTRUCTION ||
          _PyOpcode_Deopt[frame->instr_ptr->op.code] == SEND ||
          _PyOpcode_Deopt[frame->instr_ptr->op.code] == FOR_ITER ||
          _PyOpcode_Deopt[frame->instr_ptr->op.code] == INTERPRETER_EXIT ||
          _PyOpcode_Deopt[frame->instr_ptr->op.code] == ENTER_EXECUTOR);
#endif
      RELOAD_STACK();
      LOAD_IP(1 + INLINE_CACHE_ENTRIES_SEND);
      value = PyStackRef_MakeHeapSafe(temp);
      LLTRACE_RESUME_FRAME();
    }

    spilled label(start_frame) {
      CI_UPDATE_CALL_COUNT

      int too_deep = _Py_EnterRecursivePy(tstate);
      if (too_deep) {
        goto exit_unwind;
      }
      next_instr = frame->instr_ptr;
#ifdef Py_DEBUG
      int lltrace = maybe_lltrace_resume_frame(frame, GLOBALS());
      if (lltrace < 0) {
        JUMP_TO_LABEL(exit_unwind);
      }
      frame->lltrace = lltrace;
      /* _PyEval_EvalFrameDefault() must not be called with an exception set,
      because it can clear it (directly or indirectly) and so the
      caller loses its exception */
      assert(!_PyErr_Occurred(tstate));
#endif
      RELOAD_STACK();
#if Py_TAIL_CALL_INTERP
      int opcode;
#endif
      DISPATCH();
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
unbound_local_error:;
}

// Future families go below this point //
