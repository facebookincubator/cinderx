/* Copyright (c) Meta Platforms, Inc. and affiliates. */

// clang-format off
#include <Python.h>
// clang-format on

#include "frameobject.h"
#include "internal/pycore_call.h"
#include "internal/pycore_pystate.h"
#include "structmember.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/audit.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/string.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/static_array.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove
#include "cinderx/UpstreamBorrow/borrowed.h"

PyDoc_STRVAR(
    _static__doc__,
    "_static contains types related to static Python\n");

static int _static_exec(PyObject* m) {
  if (PyType_Ready(Ci_CheckedDict_Type) < 0 ||
      PyModule_AddObjectRef(m, "chkdict", (PyObject*)Ci_CheckedDict_Type) < 0) {
    return -1;
  }

  if (PyType_Ready(Ci_CheckedList_Type) < 0 ||
      PyModule_AddObjectRef(m, "chklist", (PyObject*)Ci_CheckedList_Type) < 0) {
    return -1;
  }

  if (PyType_Ready(&PyStaticArray_Type) < 0 ||
      PyModule_AddObjectRef(m, "staticarray", (PyObject*)&PyStaticArray_Type)) {
    return -1;
  }

  PyObject* type_code;
#define SET_TYPE_CODE(name)                             \
  type_code = PyLong_FromLong(name);                    \
  if (type_code == NULL) {                              \
    return -1;                                          \
  }                                                     \
  if (PyModule_AddObjectRef(m, #name, type_code) < 0) { \
    Py_DECREF(type_code);                               \
    return -1;                                          \
  }                                                     \
  Py_DECREF(type_code);

  SET_TYPE_CODE(TYPED_INT_UNSIGNED)
  SET_TYPE_CODE(TYPED_INT_SIGNED)
  SET_TYPE_CODE(TYPED_INT_8BIT)
  SET_TYPE_CODE(TYPED_INT_16BIT)
  SET_TYPE_CODE(TYPED_INT_32BIT)
  SET_TYPE_CODE(TYPED_INT_64BIT)
  SET_TYPE_CODE(TYPED_OBJECT)
  SET_TYPE_CODE(TYPED_INT8)
  SET_TYPE_CODE(TYPED_INT16)
  SET_TYPE_CODE(TYPED_INT32)
  SET_TYPE_CODE(TYPED_INT64)
  SET_TYPE_CODE(TYPED_UINT8)
  SET_TYPE_CODE(TYPED_UINT16)
  SET_TYPE_CODE(TYPED_UINT32)
  SET_TYPE_CODE(TYPED_UINT64)
  SET_TYPE_CODE(TYPED_SINGLE)
  SET_TYPE_CODE(TYPED_DOUBLE)
  SET_TYPE_CODE(TYPED_BOOL)
  SET_TYPE_CODE(TYPED_CHAR)
  SET_TYPE_CODE(TYPED_ARRAY)

  SET_TYPE_CODE(SEQ_LIST)
  SET_TYPE_CODE(SEQ_TUPLE)
  SET_TYPE_CODE(SEQ_LIST_INEXACT)
  SET_TYPE_CODE(SEQ_ARRAY_INT64)
  SET_TYPE_CODE(SEQ_SUBSCR_UNCHECKED)

  SET_TYPE_CODE(SEQ_REPEAT_INEXACT_SEQ)
  SET_TYPE_CODE(SEQ_REPEAT_INEXACT_NUM)
  SET_TYPE_CODE(SEQ_REPEAT_REVERSED)
  SET_TYPE_CODE(SEQ_REPEAT_PRIMITIVE_NUM)

  SET_TYPE_CODE(SEQ_CHECKED_LIST)

  SET_TYPE_CODE(PRIM_OP_EQ_INT)
  SET_TYPE_CODE(PRIM_OP_NE_INT)
  SET_TYPE_CODE(PRIM_OP_LT_INT)
  SET_TYPE_CODE(PRIM_OP_LE_INT)
  SET_TYPE_CODE(PRIM_OP_GT_INT)
  SET_TYPE_CODE(PRIM_OP_GE_INT)
  SET_TYPE_CODE(PRIM_OP_LT_UN_INT)
  SET_TYPE_CODE(PRIM_OP_LE_UN_INT)
  SET_TYPE_CODE(PRIM_OP_GT_UN_INT)
  SET_TYPE_CODE(PRIM_OP_GE_UN_INT)
  SET_TYPE_CODE(PRIM_OP_EQ_DBL)
  SET_TYPE_CODE(PRIM_OP_NE_DBL)
  SET_TYPE_CODE(PRIM_OP_LT_DBL)
  SET_TYPE_CODE(PRIM_OP_LE_DBL)
  SET_TYPE_CODE(PRIM_OP_GT_DBL)
  SET_TYPE_CODE(PRIM_OP_GE_DBL)

  SET_TYPE_CODE(PRIM_OP_ADD_INT)
  SET_TYPE_CODE(PRIM_OP_SUB_INT)
  SET_TYPE_CODE(PRIM_OP_MUL_INT)
  SET_TYPE_CODE(PRIM_OP_DIV_INT)
  SET_TYPE_CODE(PRIM_OP_DIV_UN_INT)
  SET_TYPE_CODE(PRIM_OP_MOD_INT)
  SET_TYPE_CODE(PRIM_OP_MOD_UN_INT)
  SET_TYPE_CODE(PRIM_OP_POW_INT)
  SET_TYPE_CODE(PRIM_OP_POW_UN_INT)
  SET_TYPE_CODE(PRIM_OP_LSHIFT_INT)
  SET_TYPE_CODE(PRIM_OP_RSHIFT_INT)
  SET_TYPE_CODE(PRIM_OP_RSHIFT_UN_INT)
  SET_TYPE_CODE(PRIM_OP_XOR_INT)
  SET_TYPE_CODE(PRIM_OP_OR_INT)
  SET_TYPE_CODE(PRIM_OP_AND_INT)

  SET_TYPE_CODE(PRIM_OP_ADD_DBL)
  SET_TYPE_CODE(PRIM_OP_SUB_DBL)
  SET_TYPE_CODE(PRIM_OP_MUL_DBL)
  SET_TYPE_CODE(PRIM_OP_DIV_DBL)
  SET_TYPE_CODE(PRIM_OP_MOD_DBL)
  SET_TYPE_CODE(PRIM_OP_POW_DBL)

  SET_TYPE_CODE(PRIM_OP_NEG_INT)
  SET_TYPE_CODE(PRIM_OP_INV_INT)
  SET_TYPE_CODE(PRIM_OP_NEG_DBL)
  SET_TYPE_CODE(PRIM_OP_NOT_INT)

  SET_TYPE_CODE(FAST_LEN_INEXACT)
  SET_TYPE_CODE(FAST_LEN_LIST)
  SET_TYPE_CODE(FAST_LEN_DICT)
  SET_TYPE_CODE(FAST_LEN_SET)
  SET_TYPE_CODE(FAST_LEN_TUPLE)
  SET_TYPE_CODE(FAST_LEN_ARRAY);
  SET_TYPE_CODE(FAST_LEN_STR)

  /* Not actually a type code, but still an int */
  SET_TYPE_CODE(RAND_MAX);

  return 0;
}

PyObject*
set_type_code(PyObject* mod, PyObject* const* args, Py_ssize_t nargs) {
  PyTypeObject* type;
  Py_ssize_t code;
  if (!_PyArg_ParseStack(args, nargs, "O!n", &PyType_Type, &type, &code)) {
    return NULL;
  } else if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
    PyErr_SetString(PyExc_TypeError, "expected heap type");
    return NULL;
  }

  _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(type, 0);
  if (vtable == NULL) {
    return NULL;
  }

  vtable->vt_typecode = code;
  Py_RETURN_NONE;
}

PyObject* is_type_static(PyObject* mod, PyObject* type) {
  PyTypeObject* pytype;
  if (!PyType_Check(type)) {
    Py_RETURN_FALSE;
  }
  pytype = (PyTypeObject*)type;
  if (pytype->tp_flags & Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* set_type_static(PyObject* mod, PyObject* type) {
  if (!PyType_Check(type)) {
    PyErr_Format(
        PyExc_TypeError,
        "Expected a type object, not %.100s",
        Py_TYPE(type)->tp_name);
    return NULL;
  }
  _PyClassLoader_SetTypeStatic((PyTypeObject*)type);
  Py_INCREF(type);
  return type;
}

PyObject* set_type_static_final(PyObject* mod, PyObject* type) {
  if (!PyType_Check(type)) {
    PyErr_Format(
        PyExc_TypeError,
        "Expected a type object, not %.100s",
        Py_TYPE(type)->tp_name);
    return NULL;
  }
  _PyClassLoader_SetTypeStatic((PyTypeObject*)type);
  ((PyTypeObject*)type)->tp_flags &= ~Py_TPFLAGS_BASETYPE;
  Py_INCREF(type);
  return type;
}

PyObject* set_type_final(PyObject* mod, PyObject* type) {
  if (!PyType_Check(type)) {
    PyErr_Format(
        PyExc_TypeError,
        "Expected a type object, not %.100s",
        Py_TYPE(type)->tp_name);
    return NULL;
  }
  PyTypeObject* pytype = (PyTypeObject*)type;
  pytype->tp_flags &= ~Py_TPFLAGS_BASETYPE;
  Py_INCREF(type);
  return type;
}

static PyObject* _recreate_cm(PyObject* self) {
  Py_INCREF(self);
  return self;
}

PyObject* make_recreate_cm(PyObject* mod, PyObject* type) {
  static PyMethodDef def = {
      "_recreate_cm", (PyCFunction)&_recreate_cm, METH_NOARGS};

  if (!PyType_Check(type)) {
    PyErr_Format(
        PyExc_TypeError,
        "Expected a type object, not %.100s",
        Py_TYPE(type)->tp_name);
    return NULL;
  }

  return PyDescr_NewMethod((PyTypeObject*)type, &def);
}

typedef struct {
  PyWeakReference weakref; /* base weak ref */
  PyObject* func; /* function that's being wrapped */
  PyObject* ctxdec; /* the instance of the ContextDecorator class */
  PyObject* enter; /* borrowed ref to __enter__, valid on cache_version */
  PyObject* exit; /* borrowed ref to __exit__, valid on cache_version */
  PyObject* recreate_cm; /* borrowed ref to recreate_cm, valid on
                            recreate_cache_version */
  Py_ssize_t cache_version;
  Py_ssize_t recreate_cache_version;
  int is_coroutine;
} _Py_ContextManagerWrapper;

static PyObject* _return_none;

int ctxmgrwrp_import_value(
    const char* module,
    const char* name,
    PyObject** dest) {
  PyObject* mod = PyImport_ImportModule(module);
  if (mod == NULL) {
    return -1;
  }
  if (*dest == NULL) {
    PyObject* value = PyObject_GetAttrString(mod, name);
    if (value == NULL) {
      return -1;
    }
    *dest = value;
  }
  Py_DECREF(mod);
  return 0;
}

static PyObject* ctxmgrwrp_exit(
    int is_coroutine,
    PyObject* ctxmgr,
    PyObject* result,
    PyObject* exit) {
  if (result == NULL) {
    // exception
    PyObject* ret;
    PyObject *exc, *val, *tb;
    PyFrameObject* f = PyEval_GetFrame();
    PyTraceBack_Here(f);
    PyErr_Fetch(&exc, &val, &tb);
    PyErr_NormalizeException(&exc, &val, &tb);
    if (tb == NULL) {
      tb = Py_None;
      Py_INCREF(tb);
    }
    PyException_SetTraceback(val, tb);

    if (ctxmgr != NULL) {
      assert(Py_TYPE(exit)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR);
      PyObject* stack[] = {(PyObject*)ctxmgr, exc, val, tb};
      ret = _PyObject_Vectorcall(exit, stack, 4, NULL);
    } else {
      PyObject* stack[] = {exc, val, tb};
      ret = _PyObject_Vectorcall(exit, stack, 3, NULL);
    }
    if (ret == NULL) {
      Py_DECREF(exc);
      Py_DECREF(val);
      Py_DECREF(tb);
      return NULL;
    }

    int err = PyObject_IsTrue(ret);
    Py_DECREF(ret);
    if (!err) {
      PyErr_Restore(exc, val, tb);
      goto error;
    }

    Py_DECREF(exc);
    Py_DECREF(val);
    Py_DECREF(tb);
    if (err < 0) {
      goto error;
    }

    if (is_coroutine) {
      /* The co-routine needs to yield None instead of raising the exception. We
       * need to actually produce a co-routine which is going to return None to
       * do that, so we have a helper function which does just that. */
      if (_return_none == NULL &&
          ctxmgrwrp_import_value("__static__", "_return_none", &_return_none)) {
        return NULL;
      }

      return _PyObject_CallNoArgs(_return_none);
    }
    Py_RETURN_NONE;
  } else {
    PyObject* ret;
    if (ctxmgr != NULL) {
      /* we picked up a method like object and have self for it */
      assert(Py_TYPE(exit)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR);
      PyObject* stack[] = {(PyObject*)ctxmgr, Py_None, Py_None, Py_None};
      ret = _PyObject_Vectorcall(exit, stack, 4, NULL);
    } else {
      PyObject* stack[] = {Py_None, Py_None, Py_None};
      ret = _PyObject_Vectorcall(exit, stack, 3, NULL);
    }
    if (ret == NULL) {
      goto error;
    }
    Py_DECREF(ret);
  }

  return result;
error:
  Py_XDECREF(result);
  return NULL;
}

static PyObject* ctxmgrwrp_cb(
    _PyClassLoader_Awaitable* awaitable,
    PyObject* result) {
  /* In the error case our awaitable is done, and if we return a value
   * it'll turn into the returned value, so we don't want to pass iscoroutine
   * because we don't need a wrapper object. */
  if (awaitable->onsend != NULL) {
    /* Send has never happened, so we never called __enter__, so there's
     * no __exit__ to call. */
    return NULL;
  }
  return ctxmgrwrp_exit(result != NULL, NULL, result, awaitable->state);
}

extern int _PyObject_GetMethod(PyObject*, PyObject*, PyObject**);

static PyObject* get_descr(PyObject* obj, PyObject* self) {
  descrgetfunc f = Py_TYPE(obj)->tp_descr_get;
  if (f != NULL) {
    return f(obj, self, (PyObject*)Py_TYPE(self));
  }
  Py_INCREF(obj);
  return obj;
}

static PyObject*
call_with_self(PyThreadState* tstate, PyObject* func, PyObject* self) {
  if (Py_TYPE(func)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR) {
    PyObject* args[1] = {self};
    return _PyObject_VectorcallTstate(tstate, func, args, 1, NULL);
  } else {
    func = get_descr(func, self);
    if (func == NULL) {
      return NULL;
    }
    PyObject* ret = _PyObject_VectorcallTstate(tstate, func, NULL, 0, NULL);
    Py_DECREF(func);
    return ret;
  }
}

static PyObject* ctxmgrwrp_enter(
    _Py_ContextManagerWrapper* self,
    PyObject** ctxmgr) {
  DEFINE_STATIC_STRING(__enter__);
  DEFINE_STATIC_STRING(__exit__);
  DEFINE_STATIC_STRING(_recreate_cm);

  PyThreadState* tstate = _PyThreadState_GET();

  if (self->recreate_cache_version != Py_TYPE(self->ctxdec)->tp_version_tag) {
    self->recreate_cm = _PyType_Lookup(Py_TYPE(self->ctxdec), s__recreate_cm);
    if (self->recreate_cm == NULL) {
      PyErr_Format(
          PyExc_TypeError,
          "failed to resolve _recreate_cm on %s",
          Py_TYPE(self->ctxdec)->tp_name);
      return NULL;
    }

    self->recreate_cache_version = Py_TYPE(self->ctxdec)->tp_version_tag;
  }

  PyObject* ctx_mgr = call_with_self(tstate, self->recreate_cm, self->ctxdec);
  if (ctx_mgr == NULL) {
    return NULL;
  }

  if (self->cache_version != Py_TYPE(ctx_mgr)->tp_version_tag) {
    /* we probably get the same type back from _recreate_cm over and
     * over again, so we cache the lookups for enter and exit */
    self->enter = _PyType_Lookup(Py_TYPE(ctx_mgr), s___enter__);
    self->exit = _PyType_Lookup(Py_TYPE(ctx_mgr), s___exit__);
    if (self->enter == NULL || self->exit == NULL) {
      Py_DECREF(ctx_mgr);
      PyErr_Format(
          PyExc_TypeError,
          "failed to resolve context manager on %s",
          Py_TYPE(ctx_mgr)->tp_name);
      return NULL;
    }

    self->cache_version = Py_TYPE(ctx_mgr)->tp_version_tag;
  }

  PyObject* enter = self->enter;
  PyObject* exit = self->exit;

  Py_INCREF(enter);
  if (!(Py_TYPE(exit)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR)) {
    /* Descriptor protocol for exit needs to run before we call
     * user code */
    exit = get_descr(exit, ctx_mgr);
    Py_CLEAR(ctx_mgr);
    if (exit == NULL) {
      return NULL;
    }
  } else {
    Py_INCREF(exit);
  }

  PyObject* enter_res = call_with_self(tstate, enter, ctx_mgr);
  Py_DECREF(enter);

  if (enter_res == NULL) {
    goto error;
  }
  Py_DECREF(enter_res);

  *ctxmgr = ctx_mgr;
  return exit;
error:
  Py_DECREF(ctx_mgr);
  return NULL;
}

static int ctxmgrwrp_first_send(_PyClassLoader_Awaitable* self) {
  /* Handles calling __enter__ on the first step of the co-routine when
   * we're not eagerly evaluated. We'll swap our state over to the exit
   * function from the _Py_ContextManagerWrapper once we're successful */
  _Py_ContextManagerWrapper* ctxmgrwrp =
      (_Py_ContextManagerWrapper*)self->state;
  PyObject* ctx_mgr;
  PyObject* exit = ctxmgrwrp_enter(ctxmgrwrp, &ctx_mgr);
  Py_DECREF(ctxmgrwrp);
  if (exit == NULL) {
    return -1;
  }
  if (ctx_mgr != NULL) {
    PyObject* bound_exit = get_descr(exit, ctx_mgr);
    if (bound_exit == NULL) {
      return -1;
    }
    Py_DECREF(exit);
    Py_DECREF(ctx_mgr);
    exit = bound_exit;
  }
  self->state = exit;
  return 0;
}

static PyObject* ctxmgrwrp_make_awaitable(
    _Py_ContextManagerWrapper* ctxmgrwrp,
    PyObject* ctx_mgr,
    PyObject* exit,
    PyObject* res,
    int eager) {
  /* We won't have exit yet if we're not eagerly evaluated, and haven't called
   * __enter__ yet.  In that case we'll setup ctxmgrwrp_first_send to run on
   * the first iteration (with the wrapper as our state)) and then restore the
   * awaitable wrapper to our normal state of having exit as the state after
   * we've called __enter__ */
  if (ctx_mgr != NULL && exit != NULL) {
    PyObject* bound_exit = get_descr(exit, ctx_mgr);
    if (bound_exit == NULL) {
      return NULL;
    }
    Py_DECREF(exit);
    Py_DECREF(ctx_mgr);
    exit = bound_exit;
  }
  res = _PyClassLoader_NewAwaitableWrapper(
      res,
      eager,
      exit == NULL ? (PyObject*)ctxmgrwrp : exit,
      ctxmgrwrp_cb,
      exit == NULL ? ctxmgrwrp_first_send : NULL);
  Py_XDECREF(exit);
  return res;
}

PyTypeObject _PyContextDecoratorWrapper_Type;

#if PY_VERSION_HEX < 0x030C0000
#define IS_AWAITED(nargsf) (nargsf & Ci_Py_AWAITED_CALL_MARKER)
#else
#define IS_AWAITED(nargsf) false
#endif

static PyObject* ctxmgrwrp_vectorcall(
    PyFunctionObject* func,
    PyObject* const* args,
    Py_ssize_t nargsf,
    PyObject* kwargs) {
  PyWeakReference* wr = (PyWeakReference*)func->func_weakreflist;
  while (wr != NULL && Py_TYPE(wr) != &_PyContextDecoratorWrapper_Type) {
    wr = wr->wr_next;
  }
  if (wr == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "missing weakref");
    return NULL;
  }
  _Py_ContextManagerWrapper* self = (_Py_ContextManagerWrapper*)wr;

  PyObject* ctx_mgr = NULL;
  PyObject* exit = NULL;

  /* If this is a co-routine, and we're not being eagerly evaluated, we cannot
   * start calling __enter__ just yet.  We'll delay that until the first step
   * of the coroutine.  Otherwise we're not a co-routine or we're eagerly
   * awaited in which case we'll call __enter__ now and capture __exit__
   * before any possible side effects to match the normal eval loop */
  if (!self->is_coroutine || IS_AWAITED(nargsf)) {
    exit = ctxmgrwrp_enter(self, &ctx_mgr);
    if (exit == NULL) {
      return NULL;
    }
  }

  /* Call the wrapped function */
  PyObject* res = _PyObject_Vectorcall(self->func, args, nargsf, kwargs);
  /* TODO(T128335015): Enable this when we have async/await support. */
  if (self->is_coroutine && res != NULL) {
#if PY_VERSION_HEX < 0x030C0000
    /* If it's a co-routine either pass up the eagerly awaited value or
     * pass out a wrapping awaitable */
    int eager = Ci_PyWaitHandle_CheckExact(res);
    if (eager) {
      Ci_PyWaitHandleObject* handle = (Ci_PyWaitHandleObject*)res;
      if (handle->wh_waiter == NULL) {
        assert(nargsf & Ci_Py_AWAITED_CALL_MARKER && exit != NULL);
        // pass in unwrapped result into exit so it could be released in error
        // case
        PyObject* result =
            ctxmgrwrp_exit(1, ctx_mgr, handle->wh_coro_or_result, exit);
        Py_DECREF(exit);
        Py_XDECREF(ctx_mgr);
        if (result == NULL) {
          // wrapped result is released in ctxmgrwrp_exit, now release the
          // waithandle itself
          Ci_PyWaitHandle_Release((PyObject*)handle);
          return NULL;
        }
        return res;
      }
    }
#else
    int eager = 0;
#endif
    return ctxmgrwrp_make_awaitable(self, ctx_mgr, exit, res, eager);
  }

  if (exit == NULL) {
    assert(self->is_coroutine && res == NULL);
    /* We must have failed producing the coroutine object for the
     * wrapped function, we haven't called __enter__, just report
     * out the error from creating the co-routine */
    return NULL;
  }

  /* Call __exit__ */
  res = ctxmgrwrp_exit(self->is_coroutine, ctx_mgr, res, exit);
  Py_XDECREF(ctx_mgr);
  Py_DECREF(exit);
  return res;
}

static int ctxmgrwrp_traverse(
    _Py_ContextManagerWrapper* self,
    visitproc visit,
    void* arg) {
  _PyWeakref_RefType.tp_traverse((PyObject*)self, visit, arg);
  Py_VISIT(self->ctxdec);
  return 0;
}

static int ctxmgrwrp_clear(_Py_ContextManagerWrapper* self) {
  _PyWeakref_RefType.tp_clear((PyObject*)self);
  Py_CLEAR(self->ctxdec);
  return 0;
}

static void ctxmgrwrp_dealloc(_Py_ContextManagerWrapper* self) {
  ctxmgrwrp_clear(self);
  _PyWeakref_RefType.tp_dealloc((PyObject*)self);
}

PyTypeObject _PyContextDecoratorWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "context_decorator_wrapper",
    sizeof(_Py_ContextManagerWrapper),
    .tp_base = &_PyWeakref_RefType,
    .tp_dealloc = (destructor)ctxmgrwrp_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)ctxmgrwrp_traverse,
    .tp_clear = (inquiry)ctxmgrwrp_clear,
};

static PyObject* weakref_callback_impl(
    PyObject* self,
    _Py_ContextManagerWrapper* weakref) {
  /* the weakref provides a callback when the object it's tracking
     is freed.  The only thing holding onto this weakref is the
     function object we're tracking, so we rely upon this callback
     to free the weakref / context mgr wrapper. */
  Py_DECREF(weakref);

  Py_RETURN_NONE;
}

static PyMethodDef _WeakrefCallback = {
    "weakref_callback",
    (PyCFunction)weakref_callback_impl,
    METH_O,
    NULL};

static PyObject* weakref_callback;

PyObject* make_context_decorator_wrapper(
    PyObject* mod,
    PyObject* const* args,
    Py_ssize_t nargs) {
  if (nargs != 3) {
    PyErr_SetString(
        PyExc_TypeError,
        "expected 3 arguments: context decorator, wrapper func, and original "
        "func");
    return NULL;
  } else if (PyType_Ready(&_PyContextDecoratorWrapper_Type)) {
    return NULL;
  } else if (!PyFunction_Check(args[1])) {
    PyErr_SetString(PyExc_TypeError, "expected function for argument 2");
    return NULL;
  }

  PyFunctionObject* wrapper_func = (PyFunctionObject*)args[1];
  PyObject* wrapped_func = args[2];

  if (weakref_callback == NULL) {
    weakref_callback = PyCFunction_New(&_WeakrefCallback, NULL);
    if (weakref_callback == NULL) {
      return NULL;
    }
  }

  PyObject* wrargs = PyTuple_New(2);
  if (wrargs == NULL) {
    return NULL;
  }

  PyTuple_SET_ITEM(wrargs, 0, (PyObject*)wrapper_func);
  Py_INCREF(wrapper_func);
  PyTuple_SET_ITEM(wrargs, 1, weakref_callback);
  Py_INCREF(weakref_callback);

  _Py_ContextManagerWrapper* ctxmgr_wrapper =
      (_Py_ContextManagerWrapper*)_PyWeakref_RefType.tp_new(
          &_PyContextDecoratorWrapper_Type, wrargs, NULL);
  Py_DECREF(wrargs);

  if (ctxmgr_wrapper == NULL) {
    return NULL;
  }

  ctxmgr_wrapper->recreate_cache_version = -1;
  ctxmgr_wrapper->cache_version = -1;
  ctxmgr_wrapper->enter = ctxmgr_wrapper->exit = ctxmgr_wrapper->recreate_cm =
      NULL;
  ctxmgr_wrapper->ctxdec = args[0];
  Py_INCREF(args[0]);
  ctxmgr_wrapper->func = wrapped_func; /* borrowed, the weak ref will live as
                                          long as the function */
  ctxmgr_wrapper->is_coroutine =
      ((PyCodeObject*)wrapper_func->func_code)->co_flags & CO_COROUTINE;

  wrapper_func->func_weakreflist = (PyObject*)ctxmgr_wrapper;
  wrapper_func->vectorcall = (vectorcallfunc)ctxmgrwrp_vectorcall;

  Py_INCREF(wrapper_func);
  return (PyObject*)wrapper_func;
}

#if PY_VERSION_HEX < 0x030C0000

static int64_t static_rand(PyObject* self) {
  return rand();
}

Ci_Py_TYPED_SIGNATURE(static_rand, Ci_Py_SIG_INT32, NULL);

static int64_t posix_clock_gettime_ns(PyObject* mod) {
  struct timespec result;
  int64_t ret;

  clock_gettime(CLOCK_MONOTONIC, &result);
  ret = result.tv_sec * 1e9 + result.tv_nsec;
  return ret;
}

Ci_Py_TYPED_SIGNATURE(posix_clock_gettime_ns, Ci_Py_SIG_INT64, NULL);

static Py_ssize_t static_property_missing_fget(PyObject* mod, PyObject* self) {
  PyErr_SetString(PyExc_AttributeError, "unreadable attribute");
  return -1;
}

Ci_Py_TYPED_SIGNATURE(
    static_property_missing_fget,
    Ci_Py_SIG_ERROR,
    &Ci_Py_Sig_Object,
    NULL);

#if PY_VERSION_HEX < 0x030C0000
static Py_ssize_t
static_property_missing_fset(PyObject* mod, PyObject* self, PyObject* val) {
  PyErr_SetString(PyExc_AttributeError, "can't set attribute");
  return -1;
}

Ci_Py_TYPED_SIGNATURE(
    static_property_missing_fset,
    Ci_Py_SIG_ERROR,
    &Ci_Py_Sig_Object,
    &Ci_Py_Sig_Object,
    NULL);
#else
static_property_missing_fset(
    PyObject* mod,
    PyObject* const* args,
    Py_ssize_t nargs) {
  PyErr_SetString(PyExc_AttributeError, "can't set attribute");
  return -1;
}
#endif

static Py_ssize_t static_property_missing_fdel(PyObject* mod, PyObject* self) {
  PyErr_SetString(PyExc_AttributeError, "can't del attribute");
  return -1;
}

Ci_Py_TYPED_SIGNATURE(
    static_property_missing_fdel,
    Ci_Py_SIG_ERROR,
    &Ci_Py_Sig_Object,
    NULL);

#else

static PyObject* static_rand(PyObject* self) {
  return PyLong_FromLong(rand());
}

static PyObject* posix_clock_gettime_ns(PyObject* mod) {
  struct timespec result;
  int64_t ret;

  clock_gettime(CLOCK_MONOTONIC, &result);
  ret = result.tv_sec * 1e9 + result.tv_nsec;
  return PyLong_FromLong(ret);
}

static PyObject* static_property_missing_fget(PyObject* mod, PyObject* self) {
  PyErr_SetString(PyExc_AttributeError, "unreadable attribute");
  return NULL;
}

static PyObject*
static_property_missing_fset(PyObject* mod, PyObject* self, PyObject* val) {
  PyErr_SetString(PyExc_AttributeError, "can't set attribute");
  return NULL;
}

static PyObject* static_property_missing_fdel(PyObject* mod, PyObject* self) {
  PyErr_SetString(PyExc_AttributeError, "can't del attribute");
  return NULL;
}

#endif

static int create_overridden_slot_descriptors_with_default(PyTypeObject* type) {
  PyObject* mro = type->tp_mro;
  if (mro == NULL) {
    return 0;
  }
  Py_ssize_t mro_size = PyTuple_GET_SIZE(mro);
  if (mro_size <= 1) {
    return 0;
  }

  PyObject* slots_with_default = NULL;
  PyTypeObject* next;
  for (Py_ssize_t i = 1; i < mro_size; i++) {
    next = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
    if (!(PyType_HasFeature(next, Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED))) {
      continue;
    }
    assert(_PyType_GetDict(next) != NULL);
    slots_with_default =
        PyDict_GetItemString(_PyType_GetDict(next), "__slots_with_default__");
    break;
  }
  if (slots_with_default == NULL) {
    // Any class built before `__build_class__` is patched won't have a
    // slots_with_default. In order to support bootstrapping, silently allow
    // that to go through.
    return 0;
  }
  if (!PyDict_CheckExact(slots_with_default)) {
    PyErr_Format(
        PyExc_TypeError,
        "The `__slots_with_default__` attribute of the class `%s` is not a "
        "dict.",
        type->tp_name);
    return -1;
  }
  PyObject* tp_dict = _PyType_GetDict(type);
  PyObject* type_slots =
      PyDict_GetItemString(tp_dict, "__slots_with_default__");
  if (type_slots == NULL) {
    type_slots = tp_dict;
  }
  Py_ssize_t i = 0;
  PyObject *name, *default_value;
  while (PyDict_Next(slots_with_default, &i, &name, &default_value)) {
    PyObject* override = PyDict_GetItem(tp_dict, name);
    if (override != NULL && Py_TYPE(override)->tp_descr_get != NULL) {
      // If the subclass overrides the base slot with a descriptor, just leave
      // it be.
      continue;
    }
    PyObject* override_default = NULL;
    if (type_slots != NULL &&
        (override_default = PyDict_GetItem(type_slots, name)) != NULL) {
      default_value = override_default;
    }
    PyObject* typed_descriptor = _PyType_Lookup(next, name);
    if (typed_descriptor == NULL ||
        Py_TYPE(typed_descriptor) != &_PyTypedDescriptorWithDefaultValue_Type) {
      PyErr_Format(
          PyExc_TypeError,
          "The slot at %R is not a typed descriptor for class `%s`.",
          name,
          next->tp_name);
      return -1;
    }
    _PyTypedDescriptorWithDefaultValue* td =
        (_PyTypedDescriptorWithDefaultValue*)typed_descriptor;
    PyObject* new_typed_descriptor = _PyTypedDescriptorWithDefaultValue_New(
        td->td_name, td->td_type, td->td_offset, default_value);
    PyDict_SetItem(tp_dict, name, new_typed_descriptor);
    Py_DECREF(new_typed_descriptor);
  }
  return 0;
}

static PyObject* init_subclass(PyObject* self, PyObject* type) {
  if (!PyType_Check(type)) {
    PyErr_SetString(PyExc_TypeError, "init_subclass expected type");
    return NULL;
  }
  // Validate that no Static Python final methods are overridden.
  PyTypeObject* typ = (PyTypeObject*)type;
  if (_PyClassLoader_IsFinalMethodOverridden(
          typ->tp_base, _PyType_GetDict(typ))) {
    return NULL;
  }
  if (create_overridden_slot_descriptors_with_default(typ) < 0) {
    return NULL;
  }
  Py_RETURN_NONE;
}

// Gets the __build_class__ builtin so that we can defer class creation to it.
// Returns a new reference.
static PyObject* get_build_class() {
  DEFINE_STATIC_STRING(__build_class__);
  PyObject* bltins = PyEval_GetBuiltins();
  PyObject* bc;
  if (PyDict_CheckExact(bltins)) {
    bc = PyDict_GetItemWithError(bltins, s___build_class__);
    if (bc == NULL) {
      if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_NameError, "__build_class__ not found");
      }
      return NULL;
    }
    Py_INCREF(bc);
  } else {
    bc = PyObject_GetItem(bltins, s___build_class__);
    if (bc == NULL) {
      if (PyErr_ExceptionMatches(PyExc_KeyError))
        PyErr_SetString(PyExc_NameError, "__build_class__ not found");
      return NULL;
    }
  }
  return bc;
}

static int parse_slot_type(PyObject* name, Py_ssize_t* size) {
  int primitive = _PyClassLoader_ResolvePrimitiveType(name);

  // In order to support forward references, we can't resolve non-primitive
  // types and verify they are valid at this point, we just assume any
  // non-primitive is an object type.
  if (primitive == -1) {
    PyErr_Clear();
    primitive = TYPED_OBJECT;
  }
  *size = _PyClassLoader_PrimitiveTypeToSize(primitive);
  return _PyClassLoader_PrimitiveTypeToStructMemberType(primitive);
}

PyObject* get_sortable_slot(
    PyTypeObject* type,
    PyObject* name,
    PyObject* slot_type_descr) {
  PyObject* slot;
  Py_ssize_t slot_size = sizeof(Py_ssize_t);
  PyObject* size_original = PyTuple_New(2);
  if (size_original == NULL) {
    return NULL;
  }

  if (slot_type_descr == NULL) {
    slot_size = sizeof(PyObject*);
    slot_type_descr = PyTuple_New(0);
    if (slot_type_descr == NULL) {
      goto error;
    }
  } else {
    int slot_type = parse_slot_type(slot_type_descr, &slot_size);
    if (slot_type == -1) {
      goto error;
    }

    slot = PyDict_GetItem(_PyType_GetDict(type), name);
    if (slot == NULL) {
      PyErr_SetString(PyExc_RuntimeError, "missing slot\n");
      goto error;
    }
    Py_INCREF(slot_type_descr);
  }

  PyObject* name_and_type_descr = PyTuple_New(2);
  if (name_and_type_descr == NULL) {
    Py_DECREF(slot_type_descr);
    goto error;
  }

  Py_INCREF(name);
  PyTuple_SET_ITEM(name_and_type_descr, 0, name);
  PyTuple_SET_ITEM(name_and_type_descr, 1, slot_type_descr);
  slot = name_and_type_descr;

  // We negate slot slot size here so that when we sort the
  // slots the largest members will come first and we naturally
  // get good alignment.  This also allows a single sort which
  // preserves the alphabetical order of slots as well as long as
  // they're the same size.
  PyObject* slot_size_obj = PyLong_FromLong(-slot_size);
  if (slot_size_obj == NULL) {
    Py_DECREF(name_and_type_descr);
    goto error;
  }
  PyTuple_SET_ITEM(size_original, 0, slot_size_obj);
  PyTuple_SET_ITEM(size_original, 1, slot);
  return size_original;
error:
  Py_DECREF(size_original);
  return NULL;
}

#if PY_VERSION_HEX >= 0x030C0000
#define PyHeapType_GET_MEMBERS(type) \
  (PyMemberDef*)PyObject_GetItemData((PyObject*)type);
#endif

static int type_new_descriptors(
    const PyObject* slots,
    PyTypeObject* type,
    int leaked_type) {
  PyHeapTypeObject* et = (PyHeapTypeObject*)type;
  Py_ssize_t slotoffset = type->tp_base->tp_basicsize;
  PyObject* dict = _PyType_GetDict(type);
  int needs_gc = (type->tp_base->tp_flags & Py_TPFLAGS_HAVE_GC) !=
      0; /* non-primitive fields require GC */

  DEFINE_STATIC_STRING(__slots_with_default__);
  PyObject* slots_with_default =
      PyDict_GetItemWithError(dict, s___slots_with_default__);
  if (slots_with_default == NULL && PyErr_Occurred()) {
    return -1;
  }

  Py_ssize_t nslot = PyTuple_GET_SIZE(slots);
  for (Py_ssize_t i = 0; i < nslot; i++) {
    PyObject* name = PyTuple_GET_ITEM(et->ht_slots, i);
    int slottype;
    Py_ssize_t slotsize;
    if (PyUnicode_Check(name)) {
      needs_gc = 1;
      slottype = T_OBJECT_EX;
      slotsize = sizeof(PyObject*);
    } else if (Py_SIZE(PyTuple_GET_ITEM(name, 1)) == 0) {
      needs_gc = 1;
      slottype = T_OBJECT_EX;
      slotsize = sizeof(PyObject*);
      name = PyTuple_GET_ITEM(name, 0);
    } else {
      /* TODO: it'd be nice to unify with the calls above */
      slottype = parse_slot_type(PyTuple_GET_ITEM(name, 1), &slotsize);
      assert(slottype != -1);
      if (slottype == T_OBJECT_EX) {
        /* Add strongly typed reference type descriptor,
         * add_members will check and not overwrite this new
         * descriptor  */
        PyObject* default_value = NULL;
        if (slots_with_default != NULL) {
          default_value = PyDict_GetItemWithError(
              slots_with_default, PyTuple_GET_ITEM(name, 0));
        }
        if (default_value == NULL && PyErr_Occurred()) {
          return -1;
        }
        PyObject* descr;
        if (default_value != NULL) {
          descr = _PyTypedDescriptorWithDefaultValue_New(
              PyTuple_GET_ITEM(name, 0),
              PyTuple_GET_ITEM(name, 1),
              slotoffset,
              default_value);
        } else {
          descr = _PyTypedDescriptor_New(
              PyTuple_GET_ITEM(name, 0), PyTuple_GET_ITEM(name, 1), slotoffset);
        }

        if (descr == NULL ||
            PyDict_SetItem(dict, PyTuple_GET_ITEM(name, 0), descr)) {
          return -1;
        }
        Py_DECREF(descr);

        if (!needs_gc) {
          int optional, exact;
          PyTypeObject* resolved_type = _PyClassLoader_ResolveType(
              PyTuple_GET_ITEM(name, 1), &optional, &exact);

          if (resolved_type == NULL) {
            /* this can fail if the type isn't loaded yet, in which case
             * we need to be pessimistic about whether or not this type
             * needs gc */
            PyErr_Clear();
          }

          if (resolved_type == NULL ||
              resolved_type->tp_flags &
                  (Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE)) {
            needs_gc = 1;
          }
          Py_XDECREF(resolved_type);
        }
      }

      name = PyTuple_GET_ITEM(name, 0);
    }

    // find the member that we're updating...  By default we do the base
    // initialization with all of the slots defined, and we're just changing
    // their types and moving them around.
    PyMemberDef* mp = PyHeapType_GET_MEMBERS(et);
    const char* slot_name = PyUnicode_AsUTF8(name);
    for (Py_ssize_t i = 0; i < nslot; i++, mp++) {
      if (strcmp(slot_name, mp->name) == 0) {
        break;
      }
    }

    if (leaked_type && (mp->type != slottype || mp->offset != slotoffset)) {
      // We can't account for all of the references to this type, which
      // means an instance of the type was created, and now we're changing
      // the layout which is dangerous.  Disallow the type definition.
      goto leaked_error;
    }

    mp->type = slottype;
    mp->offset = slotoffset;

    /* __dict__ and __weakref__ are already filtered out */
    assert(strcmp(mp->name, "__dict__") != 0);
    assert(strcmp(mp->name, "__weakref__") != 0);

    slotoffset += slotsize;
  }
  /* Round slotoffset up so any child class layouts start properly aligned. */
  slotoffset = _Py_SIZE_ROUND_UP(slotoffset, sizeof(PyObject*));

#if PY_VERSION_HEX >= 0x030C0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_PREHEADER))
#endif
  {
    if (type->tp_dictoffset) {
      if (type->tp_base->tp_itemsize == 0) {
        type->tp_dictoffset = slotoffset;
      }
      slotoffset += sizeof(PyObject*);
      needs_gc = 1;
    }

    if (type->tp_weaklistoffset) {
      type->tp_weaklistoffset = slotoffset;
      slotoffset += sizeof(PyObject*);
      needs_gc = 1;
    }
#if PY_VERSION_HEX >= 0x030C0000
  } else {
    needs_gc = 1;
#endif
  }

  // We should have checked for leakage earlier...
  if (leaked_type && type->tp_basicsize != slotoffset) {
    goto leaked_error;
  }

  type->tp_basicsize = slotoffset;
  if (!needs_gc) {
    assert(!leaked_type);
    type->tp_flags &= ~Py_TPFLAGS_HAVE_GC;
    // If we don't have GC then our base doesn't either, and we
    // need to undo the switch over to PyObject_GC_Del.
    type->tp_free = type->tp_base->tp_free;
  }
  return 0;

leaked_error:
  PyErr_SetString(
      PyExc_RuntimeError,
      "type has leaked, make sure no instances "
      "were created before the class initialization "
      "was completed and that a meta-class or base "
      "class did not register the type externally");
  return -1;
}

int init_static_type(PyObject* obj, int leaked_type) {
  PyTypeObject* type = (PyTypeObject*)obj;
  PyMemberDef* mp = PyHeapType_GET_MEMBERS(type);
  Py_ssize_t nslot = Py_SIZE(type);

  DEFINE_STATIC_STRING(__slot_types__);
  PyObject* slot_types =
      PyDict_GetItemWithError(_PyType_GetDict(type), s___slot_types__);
  if (PyErr_Occurred()) {
    return -1;
  }
  if (slot_types != NULL) {
    if (!PyDict_CheckExact(slot_types)) {
      PyErr_Format(PyExc_TypeError, "__slot_types__ should be a dict");
      return -1;
    }
    if (PyDict_GetItemString(slot_types, "__dict__") ||
        PyDict_GetItemString(slot_types, "__weakref__")) {
      PyErr_Format(
          PyExc_TypeError,
          "__slots__ type spec cannot be provided for "
          "__weakref__ or __dict__");
      return -1;
    }

    PyObject* new_slots = PyList_New(nslot);

    for (Py_ssize_t i = 0; i < nslot; i++, mp++) {
      PyObject* name = PyUnicode_FromString(mp->name);
      PyObject* slot_type_descr = PyDict_GetItem(slot_types, name);
      PyObject* size_original = get_sortable_slot(type, name, slot_type_descr);
      Py_DECREF(name);
      if (size_original == NULL) {
        Py_DECREF(new_slots);
        return -1;
      }

      PyList_SET_ITEM(new_slots, i, size_original);
    }

    if (PyList_Sort(new_slots) == -1) {
      Py_DECREF(new_slots);
      return -1;
    }

    /* convert back to the original values */
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(new_slots); i++) {
      PyObject* val = PyList_GET_ITEM(new_slots, i);

      PyObject* original = PyTuple_GET_ITEM(val, PyTuple_GET_SIZE(val) - 1);
      Py_INCREF(original);
      PyList_SET_ITEM(new_slots, i, original);
      Py_DECREF(val);
    }

    PyObject* tuple = PyList_AsTuple(new_slots);
    Py_DECREF(new_slots);
    if (tuple == NULL) {
      return -1;
    }

    Py_SETREF(((PyHeapTypeObject*)type)->ht_slots, tuple);

    if (type_new_descriptors(tuple, type, leaked_type)) {
      return -1;
    }
  }

  if (_PyClassLoader_IsFinalMethodOverridden(
          type->tp_base, _PyType_GetDict(type))) {
    return -1;
  }

  return 0;
}

static int validate_base_types(PyTypeObject* pytype) {
  /* Inheriting a non-static type which inherits a static type is not sound, and
   * we can only catch it at runtime. The compiler can't see the static base
   * through the nonstatic type (which is opaque to it) and thus a) can't verify
   * validity of method and attribute overrides, and b) also can't check
   * statically if this case has occurred. */
  PyObject* mro = pytype->tp_mro;
  PyTypeObject* nonstatic_base = NULL;

  for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); i++) {
    PyTypeObject* next = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
    if (next->tp_flags & Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED) {
      if (nonstatic_base) {
        PyErr_Format(
            PyExc_TypeError,
            "Static compiler cannot verify that static type '%s' is a valid "
            "override of static base '%s' because intervening base '%s' is "
            "non-static.",
            pytype->tp_name,
            next->tp_name,
            nonstatic_base->tp_name);
        return -1;
      }
    } else if (nonstatic_base == NULL) {
      nonstatic_base = next;
    }
  }
  return 0;
}

static int init_cached_properties(
    PyTypeObject* type,
    PyObject* cached_properties) {
  /*
      Static Python compiles cached properties into something like this:
          class C:
              __slots__ = ("x")

              def _x_impl(self): ...

              C.x = cached_property(C._x_impl, C.x)
              del C._x_impl

      The last two lines result in a STORE_ATTR + DELETE_ATTR. However, both
     those opcodes result in us creating a v-table on the C class. That's not
     correct, because the v-table should be created only _after_ `C.x` is
     assigned (and the impl deleted).

      This function does the job, without going through the v-table creation and
     does it in bulk for all of the cached properties.
  */
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(cached_properties); i++) {
    PyObject* impl_name = PyTuple_GET_ITEM(cached_properties, i);
    if (!PyUnicode_CheckExact(impl_name)) {
      PyErr_Format(
          PyExc_TypeError,
          "illegal cached property value: %s",
          Py_TYPE(impl_name)->tp_name);
      return -1;
    }
    PyObject* impl = PyDict_GetItem(_PyType_GetDict(type), impl_name);
    if (impl == NULL) {
      PyErr_Format(
          PyExc_TypeError, "cached property impl doesn't exist: %R", impl_name);
      return -1;
    }

    const char* name = PyUnicode_AsUTF8(impl_name);
    const char* async_prefix = "_pystatic_async_cprop.";
    const char* normal_prefix = "_pystatic_cprop.";
    PyObject *property, *attr;
    if (!strncmp(name, async_prefix, strlen(async_prefix))) {
      char attr_name[strlen(name) - strlen(async_prefix) + 1];
      strcpy(attr_name, name + strlen(async_prefix));
      attr = PyUnicode_FromString(attr_name);
      PyObject* descr = PyDict_GetItem(_PyType_GetDict(type), attr);
      if (descr == NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "cached property descriptor doesn't exist: %R",
            attr);
        return -1;
      }

      PyObject* args[2];

      args[0] = impl;
      args[1] = descr;
      property = PyObject_Vectorcall(
          (PyObject*)&PyAsyncCachedProperty_Type, args, 2, NULL);
    } else if (!strncmp(name, normal_prefix, strlen(normal_prefix))) {
      char attr_name[strlen(name) - strlen(normal_prefix) + 1];
      strcpy(attr_name, name + strlen(normal_prefix));
      attr = PyUnicode_FromString(attr_name);
      PyObject* descr = PyDict_GetItem(_PyType_GetDict(type), attr);
      if (descr == NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "cached property descriptor doesn't exist: %R",
            attr);
        return -1;
      }

      PyObject* args[2];

      args[0] = impl;
      args[1] = descr;
      property =
          PyObject_Vectorcall((PyObject*)&PyCachedProperty_Type, args, 2, NULL);
    } else {
      PyErr_Format(PyExc_TypeError, "unknown prefix: %R", impl_name);
      return -1;
    }
    if (property == NULL) {
      Py_DECREF(attr);
      return -1;
    }

    // First setup the cached_property
    int res;
    res =
        _PyObject_GenericSetAttrWithDict((PyObject*)type, attr, property, NULL);
    if (res != 0) {
      return -1;
    }

    // Next clear the backing slot
    res = _PyObject_GenericSetAttrWithDict(
        (PyObject*)type, impl_name, NULL, NULL);
    if (res != 0) {
      return -1;
    }
    Py_DECREF(property);
    Py_DECREF(attr);

    PyType_Modified(type);
  }
  return 0;
}

static PyObject* _static___build_cinder_class__(
    PyObject* self,
    PyObject* const* args,
    Py_ssize_t nargs) {
  DEFINE_STATIC_STRING(__final_method_names__);

  PyObject* mkw;
  PyObject* type = NULL;
  const int min_arg_count = 7; // the minimum number of arguments we take
  const int extra_args = 5; // the number of extra arguments we take in
                            // comparison to normal __build_class__
  if (nargs < min_arg_count) {
    PyErr_SetString(
        PyExc_TypeError, "__build_cinder_class__: not enough arguments");
    return NULL;
  }

  mkw = args[2];
  if (mkw != Py_None) {
    if (!PyDict_CheckExact(mkw)) {
      PyErr_SetString(
          PyExc_TypeError,
          "__build_cinder_class__: kwargs is not a dict or None");
      return NULL;
    }
  } else {
    mkw = NULL;
  }

  int has_class_cell = PyObject_IsTrue(args[3]);
  PyObject* final_method_names = args[4];
  int final = PyObject_IsTrue(args[5]);
  PyObject* cached_properties = args[6];
  if (!PyTuple_CheckExact(cached_properties)) {
    PyErr_SetString(
        PyExc_TypeError,
        "__build_cinder_class__: cached_properties is not a tuple");
    return NULL;
  }

  PyObject* bc = get_build_class();
  if (bc == NULL) {
    return NULL;
  }

  int kwarg_count = 0;
  if (mkw != NULL) {
    kwarg_count = PyDict_GET_SIZE(mkw);
  }

  // remove the kwarg dict and add the kwargs
  PyObject* call_args[kwarg_count + nargs - 1];
  call_args[0] = args[0]; // func
  call_args[1] = args[1]; // name

  // bases are offset by extra_args due to the extra args we take
  for (Py_ssize_t i = min_arg_count; i < nargs; i++) {
    call_args[i - extra_args] = args[i];
  }

  PyObject* call_names_tuple = NULL;
  if (mkw != NULL && kwarg_count != 0) {
    PyObject* call_names[kwarg_count];
    Py_ssize_t i = 0, cur = 0;
    PyObject *key, *value;
    while (PyDict_Next(mkw, &i, &key, &value)) {
      call_args[nargs - extra_args + cur] = value;
      call_names[cur++] = key;
    }
    call_names_tuple = Cix_PyTuple_FromArray(call_names, kwarg_count);
    if (call_names_tuple == NULL) {
      goto error;
    }
  }

  type =
      PyObject_Vectorcall(bc, call_args, nargs - extra_args, call_names_tuple);
  if (type == NULL) {
    goto error;
  }

  int res;
  res = _PyObject_GenericSetAttrWithDict(
      type, s___final_method_names__, final_method_names, NULL);
  if (res != 0) {
    goto error;
  }

  PyTypeObject* pytype = (PyTypeObject*)type;
  int had_type_cache = 0;
  if (pytype->tp_cache != NULL) {
    /* If the v-table was inited because our base class was
     * already inited, it is no longer valid...  we need to include
     * statically defined methods (we'd be better off having custom
     * static class building which knows we're building a static type
     * from the get-go) */
    Py_CLEAR(((PyTypeObject*)type)->tp_cache);
    had_type_cache = 1;
  }

  if (final) {
    pytype->tp_flags &= ~Py_TPFLAGS_BASETYPE;
  }
  pytype = (PyTypeObject*)type;

  if (validate_base_types(pytype) < 0) {
    goto error;
  }

  int slot_count = 0;
  int leaked_type = 0;
  if (((PyHeapTypeObject*)type)->ht_slots != NULL) {
    slot_count = PyTuple_GET_SIZE(((PyHeapTypeObject*)type)->ht_slots);
  }

  // If we don't have any slots then there's no layout to fixup
  if (slot_count) {
    // Account for things which add extra references...
    if (has_class_cell) {
      slot_count++;
    }
    PyTypeObject* tp = (PyTypeObject*)type;
    if (tp->tp_weaklistoffset && !tp->tp_base->tp_weaklistoffset) {
      slot_count++;
    }
    if (tp->tp_dictoffset && !tp->tp_base->tp_dictoffset) {
      slot_count++;
    }
    // Type by default has 2 references, the one which we'll return, and one
    // which is a circular reference between the type and its MRO
    if (type->ob_refcnt != 2 + slot_count) {
      leaked_type = 1;
    }
  }

  if (!PyType_Check(type)) {
    PyErr_SetString(
        PyExc_TypeError, "__build_class__ returned non-type for static Python");
    goto error;
  } else if (
      init_static_type(type, leaked_type) ||
      create_overridden_slot_descriptors_with_default((PyTypeObject*)type) <
          0) {
    goto error;
  }
  if (PyTuple_GET_SIZE(cached_properties) &&
      init_cached_properties(pytype, cached_properties) < 0) {
    goto error;
  }
  if (_PyClassLoader_SetTypeStatic(pytype) < 0) {
    goto error;
  }
  // If we were subtyping a class which was known statically then the v-table
  // will be eagerly initialized before we completed the static initialization
  // of the type.  In that case we cleared out the cache earlier, and now we
  // want to make sure we have the v-table in place as there may already exist
  // invokes against the base class members that we'd be used in.
  if (had_type_cache && _PyClassLoader_EnsureVtable(pytype, 0) == NULL) {
    goto error;
  }

  Py_XDECREF(call_names_tuple);
  Py_DECREF(bc);
  return type;
error:
  Py_XDECREF(call_names_tuple);
  Py_DECREF(bc);
  Py_XDECREF(type);
  return NULL;
}

PyObject* resolve_primitive_descr(PyObject* mod, PyObject* descr) {
  int type_code = _PyClassLoader_ResolvePrimitiveType(descr);
  if (type_code < 0) {
    return NULL;
  }
  return PyLong_FromLong(type_code);
}

static PyObject* lookup_native_symbol(
    PyObject* Py_UNUSED(module),
    PyObject** args,
    Py_ssize_t nargs) {
  if (nargs != 2) {
    PyErr_SetString(
        PyExc_TypeError, "lookup_native_symbol: Expected 2 arguments");
    return NULL;
  }
  PyObject* lib_name = args[0];
  PyObject* symbol_name = args[1];
  void* addr = _PyClassloader_LookupSymbol(lib_name, symbol_name);
  if (addr == NULL) {
    return NULL;
  }
  return PyLong_FromVoidPtr(addr);
}

PyObject* sizeof_dlopen_cache(PyObject* Py_UNUSED(module)) {
  return _PyClassloader_SizeOf_DlOpen_Cache();
}

PyObject* sizeof_dlsym_cache(PyObject* Py_UNUSED(module)) {
  return _PyClassloader_SizeOf_DlSym_Cache();
}

PyObject* clear_dlopen_cache(PyObject* Py_UNUSED(module)) {
  _PyClassloader_Clear_DlOpen_Cache();
  Py_RETURN_NONE;
}

PyObject* clear_dlsym_cache(PyObject* Py_UNUSED(module)) {
  _PyClassloader_Clear_DlSym_Cache();
  Py_RETURN_NONE;
}

static int sp_audit_hook(const char* event, PyObject* args, void* data) {
  if (strcmp(event, "object.__setattr__") != 0 || PyTuple_GET_SIZE(args) != 3) {
    return 0;
  }
  PyObject* name = PyTuple_GET_ITEM(args, 1);
  if (!PyUnicode_Check(name) ||
      PyUnicode_CompareWithASCIIString(name, "__code__") != 0) {
    return 0;
  }

  PyObject* obj = PyTuple_GET_ITEM(args, 0);
  if (!PyFunction_Check(obj)) {
    return 0;
  }
  PyFunctionObject* func = (PyFunctionObject*)obj;
  if (((PyCodeObject*)func->func_code)->co_flags & CI_CO_STATICALLY_COMPILED) {
    PyErr_SetString(
        PyExc_RuntimeError, "Cannot modify __code__ of Static Python function");
    return -1;
  }
  return 0;
}

static int sp_audit_hook_installed = 0;

static PyObject* install_sp_audit_hook(PyObject* mod) {
  if (sp_audit_hook_installed) {
    Py_RETURN_NONE;
  }
  void* kData = NULL;
  if (!installAuditHook(sp_audit_hook, kData)) {
    PyErr_SetString(
        PyExc_RuntimeError, "Could not install Static Python audit hook");
    return NULL;
  }
  sp_audit_hook_installed = 1;
  Py_RETURN_NONE;
}

static PyMethodDef static_methods[] = {
    {"set_type_code",
     (PyCFunction)(void (*)(void))set_type_code,
     METH_FASTCALL,
     ""},
#if PY_VERSION_HEX < 0x030C0000
    {"rand", (PyCFunction)&static_rand_def, Ci_METH_TYPED, ""},
#else
    {"rand", (PyCFunction)&static_rand, METH_NOARGS, ""},
#endif
    {"is_type_static", (PyCFunction)(void (*)(void))is_type_static, METH_O, ""},
    {"set_type_static",
     (PyCFunction)(void (*)(void))set_type_static,
     METH_O,
     ""},
    {"set_type_static_final",
     (PyCFunction)(void (*)(void))set_type_static_final,
     METH_O,
     ""},
    {"set_type_final", (PyCFunction)(void (*)(void))set_type_final, METH_O, ""},
    {"make_recreate_cm",
     (PyCFunction)(void (*)(void))make_recreate_cm,
     METH_O,
     ""},
    {"make_context_decorator_wrapper",
     (PyCFunction)(void (*)(void))make_context_decorator_wrapper,
     METH_FASTCALL,
     ""},
#if PY_VERSION_HEX < 0x030C0000
    {"posix_clock_gettime_ns",
     (PyCFunction)&posix_clock_gettime_ns_def,
     Ci_METH_TYPED,
     "Returns time in nanoseconds as an int64. Note: Does no error checks at "
     "all."},
    {"_property_missing_fget",
     (PyCFunction)&static_property_missing_fget_def,
     Ci_METH_TYPED,
     ""},
    {"_property_missing_fset",
     (PyCFunction)&static_property_missing_fset_def,
     Ci_METH_TYPED,
     ""},
    {"_property_missing_fdel",
     (PyCFunction)&static_property_missing_fdel_def,
     Ci_METH_TYPED,
     ""},
#else
    {"posix_clock_gettime_ns",
     (PyCFunction)&posix_clock_gettime_ns,
     METH_NOARGS,
     "Returns time in nanoseconds as an int64. Note: Does no error checks at "
     "all."},
    {"_property_missing_fget",
     (PyCFunction)&static_property_missing_fget,
     METH_O,
     ""},
    {"_property_missing_fset",
     (PyCFunction)&static_property_missing_fset,
     METH_FASTCALL,
     ""},
    {"_property_missing_fdel",
     (PyCFunction)&static_property_missing_fdel,
     METH_O,
     ""},
#endif
    {"resolve_primitive_descr",
     (PyCFunction)(void (*)(void))resolve_primitive_descr,
     METH_O,
     ""},
    {"__build_cinder_class__",
     (PyCFunction)_static___build_cinder_class__,
     METH_FASTCALL,
     ""},
    {"init_subclass", (PyCFunction)init_subclass, METH_O, ""},
    {"lookup_native_symbol",
     (PyCFunction)(void (*)(void))lookup_native_symbol,
     METH_FASTCALL,
     ""},
    {"_sizeof_dlopen_cache",
     (PyCFunction)(void (*)(void))sizeof_dlopen_cache,
     METH_FASTCALL,
     ""},
    {"_sizeof_dlsym_cache",
     (PyCFunction)(void (*)(void))sizeof_dlsym_cache,
     METH_FASTCALL,
     ""},
    {"_clear_dlopen_cache",
     (PyCFunction)(void (*)(void))clear_dlopen_cache,
     METH_FASTCALL,
     ""},
    {"_clear_dlsym_cache",
     (PyCFunction)(void (*)(void))clear_dlsym_cache,
     METH_FASTCALL,
     ""},
    {"install_sp_audit_hook",
     (PyCFunction)(void (*)(void))install_sp_audit_hook,
     METH_NOARGS,
     ""},
    {}};

static struct PyModuleDef _staticmodule = {
    PyModuleDef_HEAD_INIT,
    "_static",
    _static__doc__,
    0,
    static_methods,
    NULL,
    NULL,
    NULL,
    NULL};

int _Ci_CreateStaticModule(void) {
  PyObject* mod = PyModule_Create(&_staticmodule);
  if (mod == NULL) {
    return -1;
  }

  PyObject* modname = PyUnicode_InternFromString("_static");
  if (modname == NULL) {
    Py_DECREF(mod);
    return -1;
  }

  PyObject* modules = PyImport_GetModuleDict();
  int st = _PyImport_FixupExtensionObject(mod, modname, modname, modules);
  Py_DECREF(modname);
  if (st == -1 || _static_exec(mod) < 0) {
    Py_DECREF(mod);
    return -1;
  }

  return 0;
}
