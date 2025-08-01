// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Interpreter/iter_helpers.h"

#include "cinderx/UpstreamBorrow/borrowed.h"

// clang-format off
#include "internal/pycore_pyerrors.h"

PyObject* Ci_GetAIter(PyThreadState* tstate, PyObject* obj) {
  unaryfunc getter = NULL;
  PyObject* iter = NULL;
  PyTypeObject* type = Py_TYPE(obj);

  if (type->tp_as_async != NULL) {
    getter = type->tp_as_async->am_aiter;
  }

  if (getter != NULL) {
    iter = (*getter)(obj);
    if (iter == NULL) {
      return NULL;
    }
  } else {
    _PyErr_Format(
        tstate,
        PyExc_TypeError,
        "'async for' requires an object with "
        "__aiter__ method, got %.100s",
        type->tp_name);
    return NULL;
  }

  if (Py_TYPE(iter)->tp_as_async == NULL ||
      Py_TYPE(iter)->tp_as_async->am_anext == NULL) {
    _PyErr_Format(
        tstate,
        PyExc_TypeError,
        "'async for' received an object from __aiter__ "
        "that does not implement __anext__: %.100s",
        Py_TYPE(iter)->tp_name);
    Py_DECREF(iter);
    return NULL;
  }
  return iter;
}

PyObject* Ci_GetANext(PyThreadState* tstate, PyObject* aiter) {
  unaryfunc getter = NULL;
  PyObject* next_iter = NULL;
  PyObject* awaitable = NULL;
  PyTypeObject* type = Py_TYPE(aiter);

  if (PyAsyncGen_CheckExact(aiter)) {
    awaitable = type->tp_as_async->am_anext(aiter);
    if (awaitable == NULL) {
      return NULL;
    }
  } else {
    if (type->tp_as_async != NULL) {
      getter = type->tp_as_async->am_anext;
    }

    if (getter != NULL) {
      next_iter = (*getter)(aiter);
      if (next_iter == NULL) {
        return NULL;
      }
    } else {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "'async for' requires an iterator with "
          "__anext__ method, got %.100s",
          type->tp_name);
      return NULL;
    }

    awaitable = Cix_PyCoro_GetAwaitableIter(next_iter);
    if (awaitable == NULL) {
      _PyErr_FormatFromCause(
          PyExc_TypeError,
          "'async for' received an invalid object "
          "from __anext__: %.100s",
          Py_TYPE(next_iter)->tp_name);

      Py_DECREF(next_iter);
      return NULL;
    } else {
      Py_DECREF(next_iter);
    }
  }
  return awaitable;
}
