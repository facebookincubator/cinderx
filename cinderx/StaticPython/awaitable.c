// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/awaitable.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/string.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_c_state.h"

static int
awaitable_traverse(_PyClassLoader_Awaitable* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(self->state);
  Py_VISIT(self->coro);
  Py_VISIT(self->iter);
  return 0;
}

static int awaitable_clear(_PyClassLoader_Awaitable* self) {
  Py_CLEAR(self->state);
  Py_CLEAR(self->coro);
  Py_CLEAR(self->iter);
  return 0;
}

static void awaitable_dealloc(_PyClassLoader_Awaitable* self) {
  PyTypeObject* type = Py_TYPE(self);
  PyObject_GC_UnTrack((PyObject*)self);
  awaitable_clear(self);
  type->tp_free(self);
  Py_DECREF(type);
}

static PyObject* awaitable_get_iter(_PyClassLoader_Awaitable* self) {
  PyObject* iter = _PyCoro_GetAwaitableIter(self->coro);
  if (iter == NULL) {
    return NULL;
  }

#ifdef ENABLE_GENERATOR_AWAITER
  if (self->awaiter != NULL) {
    Ci_PyAwaitable_SetAwaiter(iter, self->awaiter);
  }
#endif

  if (PyCoro_CheckExact(iter)) {
    PyObject* yf = _PyGen_yf((PyGenObject*)iter);
    if (yf != NULL) {
      Py_DECREF(yf);
      Py_DECREF(iter);
      PyErr_SetString(PyExc_RuntimeError, "coroutine is being awaited already");
      return NULL;
    }
  }
  return iter;
}

static PyObject* awaitable_await(_PyClassLoader_Awaitable* self) {
  PyObject* iter = awaitable_get_iter(self);
  if (iter == NULL) {
    return NULL;
  }
  Py_XSETREF(self->iter, iter);
  Py_INCREF(self);
  return (PyObject*)self;
}

static PySendResult awaitable_itersend(
    _PyClassLoader_Awaitable* self,
    PyObject* value,
    PyObject** pResult) {
  *pResult = NULL;

  PyObject* iter = self->iter;
  if (iter == NULL) {
    iter = awaitable_get_iter(self);
    if (iter == NULL) {
      return PYGEN_ERROR;
    }
    self->iter = iter;
  }

  if (self->onsend != NULL) {
    awaitable_presend send = self->onsend;
    self->onsend = NULL;
    if (send(self)) {
      *pResult = NULL;
      return PYGEN_ERROR;
    }
  }

  PyObject* result;

  PySendResult status = PyIter_Send(iter, value, &result);
  if (status == PYGEN_RETURN) {
    result = self->cb(self, result);
    if (result == NULL) {
      status = PYGEN_ERROR;
    }
  } else if (status == PYGEN_ERROR) {
    result = self->cb(self, NULL);
    if (result != NULL) {
      status = PYGEN_RETURN;
    }
  }

  *pResult = result;
  return status;
}

static void awaitable_setawaiter(
    _PyClassLoader_Awaitable* awaitable,
    PyObject* awaiter) {
#ifdef ENABLE_GENERATOR_AWAITER
  if (awaitable->iter != NULL) {
    Ci_PyAwaitable_SetAwaiter(awaitable->iter, awaiter);
  }
#endif

  awaitable->awaiter = awaiter;
}

#ifdef ENABLE_GENERATOR_AWAITER

static Ci_AsyncMethodsWithExtra awaitable_as_async = {
    .ame_async_methods =
        {
            .am_await = (unaryfunc)awaitable_await,
            .am_aiter = NULL,
            .am_anext = NULL,
            .am_send = (sendfunc)awaitable_itersend,
        },
    .ame_setawaiter = (setawaiterfunc)awaitable_setawaiter,
};

#else

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0

static PyAsyncMethods awaitable_as_async = {
    .am_await = (unaryfunc)awaitable_await,
    .am_aiter = NULL,
    .am_anext = NULL,
    .am_send = (sendfunc)awaitable_itersend,
};

#endif

static PyObject* awaitable_send(
    _PyClassLoader_Awaitable* self,
    PyObject* value) {
  PyObject* result;
  PySendResult status = awaitable_itersend(self, value, &result);
  if (status == PYGEN_ERROR || status == PYGEN_NEXT) {
    return result;
  }

  assert(status == PYGEN_RETURN);
  _PyGen_SetStopIterationValue(result);
  Py_DECREF(result);
  return NULL;
}

static PyObject* awaitable_next(_PyClassLoader_Awaitable* self) {
  return awaitable_send(self, Py_None);
}

static PyObject* awaitable_throw(
    _PyClassLoader_Awaitable* self,
    PyObject* args) {
  PyObject* iter = self->iter;
  if (iter == NULL) {
    iter = awaitable_get_iter(self);
    if (iter == NULL) {
      return NULL;
    }
    self->iter = iter;
  }
  DEFINE_STATIC_STRING(throw);
  PyObject* method = PyObject_GetAttr(iter, s_throw);
  if (method == NULL) {
    return NULL;
  }
  PyObject* ret = PyObject_CallObject(method, args);
  Py_DECREF(method);
  if (ret != NULL) {
    return ret;
  } else if (_PyGen_FetchStopIterationValue(&ret) < 0) {
    /* Deliver exception result to callback */
    ret = self->cb(self, NULL);
    if (ret != NULL) {
      _PyGen_SetStopIterationValue(ret);
      Py_DECREF(ret);
      return NULL;
    }
    return ret;
  }

  ret = self->cb(self, ret);
  if (ret != NULL) {
    _PyGen_SetStopIterationValue(ret);
    Py_DECREF(ret);
  }
  return NULL;
}

static PyObject* awaitable_close(
    _PyClassLoader_Awaitable* self,
    PyObject* val) {
  PyObject* iter = self->iter;
  if (iter == NULL) {
    iter = awaitable_get_iter(self);
    if (iter == NULL) {
      return NULL;
    }
    self->iter = iter;
  }
  DEFINE_STATIC_STRING(close);
  PyObject* ret = PyObject_CallMethodObjArgs(iter, s_close, val, NULL);
  Py_CLEAR(self->iter);
  return ret;
}

static PyMethodDef awaitable_methods[] = {
    {"send", (PyCFunction)awaitable_send, METH_O, NULL},
    {"throw", (PyCFunction)awaitable_throw, METH_VARARGS, NULL},
    {"close", (PyCFunction)awaitable_close, METH_NOARGS, NULL},
    {NULL, NULL},
};

static PyMemberDef awaitable_memberlist[] = {
    {"__coro__", T_OBJECT, offsetof(_PyClassLoader_Awaitable, coro), READONLY},
    {NULL} /* Sentinel */
};

static PyType_Slot awaitable_slots[] = {
    {Py_tp_dealloc, (void*)awaitable_dealloc},
    {Py_tp_traverse, (void*)awaitable_traverse},
    {Py_tp_clear, (void*)awaitable_clear},
    {Py_tp_methods, (void*)awaitable_methods},
    {Py_tp_members, (void*)awaitable_memberlist},
    {Py_tp_iter, (void*)PyObject_SelfIter},
    {Py_tp_iternext, (void*)awaitable_next},
    {Py_am_await, (void*)awaitable_await},
    {Py_am_send, (void*)awaitable_itersend},
    {0, NULL},
};

static PyType_Spec awaitable_spec = {
    .name = "_static.awaitable_wrapper",
    .basicsize = sizeof(_PyClassLoader_Awaitable),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_IMMUTABLETYPE | Ci_TPFLAGS_HAVE_AM_EXTRA,
    .slots = awaitable_slots,
};

PyObject* _PyClassLoader_NewAwaitableWrapper(
    PyObject* coro,
    int eager,
    PyObject* state,
    awaitable_cb cb,
    awaitable_presend onsend) {
  PyTypeObject* type = Ci_GetAwaitableWrapperType();
  if (type == NULL) {
    type = (PyTypeObject*)PyType_FromSpec(&awaitable_spec);
    if (type == NULL) {
      return NULL;
    }

    // PyType_FromSpec builds tp_as_async from the Py_am_* slots but doesn't
    // know about the extra setawaiter slot.  Overwrite it with our static
    // struct that includes the extra method, mirroring AsyncLazyValueCompute.
    //
    // This is a little odd in that we're mutating a type that's been marked as
    // immutable, but since nothing uses the type yet it should be safe.
    type->tp_as_async = (PyAsyncMethods*)&awaitable_as_async;
    Ci_SetAwaitableWrapperType(type);

    // Drop the reference from PyType_FromSpec(), rely on the reference in the
    // module state.
    Py_DECREF(type);
  }

  _PyClassLoader_Awaitable* awaitable =
      PyObject_GC_New(_PyClassLoader_Awaitable, type);
  if (awaitable == NULL) {
    return NULL;
  }

  Py_INCREF(state);
  awaitable->state = state;
  awaitable->cb = cb;
  awaitable->onsend = onsend;
  awaitable->awaiter = NULL;

  awaitable->coro = coro;
  awaitable->iter = NULL;
  return (PyObject*)awaitable;
}
