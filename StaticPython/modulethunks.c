/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/modulethunks.h"

#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/vtable.h"

static void set_thunk_type_error(_Py_StaticThunk* thunk, const char* msg) {
  PyObject* name = thunk->thunk_tcs.tcs_rt.rt_name;
  if (thunk->thunk_cls != NULL) {
    name = PyUnicode_FromFormat("%s.%U", thunk->thunk_cls->tp_name, name);
  }
  PyErr_Format(CiExc_StaticTypeError, msg, name);
  if (thunk->thunk_cls != NULL) {
    Py_DECREF(name);
  }
}

#if PY_VERSION_HEX >= 0x030C0000
#define Ci_Py_AWAITED_CALL_MARKER 0
#endif

static PyObject* thunk_vectorcall(
    _Py_StaticThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  PyObject* func = thunk->thunk_tcs.tcs_value;
  if (func == NULL) {
    set_thunk_type_error(thunk, "%U has been deleted from module");
    return NULL;
  }
  if (thunk->thunk_flags & Ci_FUNC_FLAGS_CLASSMETHOD) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs == 0) {
      set_thunk_type_error(thunk, "%U must be invoked with >= 1 arguments");
      return NULL;
    }

    if (thunk->thunk_flags & Ci_FUNC_FLAGS_COROUTINE) {
      return _PyClassLoader_CallCoroutine(
          (_PyClassLoader_TypeCheckThunk*)thunk, args, nargs);
    }
    PyObject* res = _PyObject_Vectorcall(func, args + 1, nargs - 1, kwnames);
    return _PyClassLoader_CheckReturnType(
        thunk->thunk_cls, res, (_PyClassLoader_RetTypeInfo*)thunk);
  }

  if (!(thunk->thunk_flags & Ci_FUNC_FLAGS_STATICMETHOD) &&
      !PyFunction_Check(func)) {
    PyObject* callable;
    if (Py_TYPE(func)->tp_descr_get != NULL) {
      PyObject* self = args[0];
      callable =
          Py_TYPE(func)->tp_descr_get(func, self, (PyObject*)Py_TYPE(self));
      if (callable == NULL) {
        return NULL;
      }
    } else {
      Py_INCREF(func);
      callable = func;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    PyObject* res =
        _PyObject_Vectorcall(callable, args + 1, (nargs - 1), kwnames);
    Py_DECREF(callable);

    if (thunk->thunk_flags & Ci_FUNC_FLAGS_COROUTINE) {
      return _PyClassLoader_NewAwaitableWrapper(
          res, 0, (PyObject*)thunk, _PyClassLoader_CheckReturnCallback, NULL);
    }
    return _PyClassLoader_CheckReturnType(
        thunk->thunk_cls, res, (_PyClassLoader_RetTypeInfo*)thunk);
  }

  if (thunk->thunk_flags & Ci_FUNC_FLAGS_COROUTINE) {
    PyObject* coro = _PyObject_Vectorcall(
        func, args, nargsf & ~Ci_Py_AWAITED_CALL_MARKER, kwnames);

    return _PyClassLoader_NewAwaitableWrapper(
        coro, 0, (PyObject*)thunk, _PyClassLoader_CheckReturnCallback, NULL);
  }

  PyObject* res = _PyObject_Vectorcall(
      func, args, nargsf & ~Ci_Py_AWAITED_CALL_MARKER, kwnames);
  return _PyClassLoader_CheckReturnType(
      thunk->thunk_cls, res, (_PyClassLoader_RetTypeInfo*)thunk);
}

_Py_StaticThunk* _PyClassLoader_GetOrMakeThunk(
    PyObject* func,
    PyObject* original,
    PyObject* container,
    PyObject* name) {
  PyObject* thunks = NULL;
  PyTypeObject* type = NULL;
  if (PyType_Check(container)) {
    type = (PyTypeObject*)container;
    _PyType_VTable* vtable = (_PyType_VTable*)type->tp_cache;
    if (vtable->vt_thunks == NULL) {
      vtable->vt_thunks = PyDict_New();
      if (vtable->vt_thunks == NULL) {
        return NULL;
      }
    }
    thunks = vtable->vt_thunks;
  } else if (Ci_StrictModule_Check(container)) {
    Ci_StrictModuleObject* mod = (Ci_StrictModuleObject*)container;
    if (mod->static_thunks == NULL) {
      mod->static_thunks = PyDict_New();
      if (mod->static_thunks == NULL) {
        return NULL;
      }
    }
    thunks = mod->static_thunks;
  }
  _Py_StaticThunk* thunk = (_Py_StaticThunk*)PyDict_GetItem(thunks, name);
  if (thunk != NULL) {
    Py_INCREF(thunk);
    return thunk;
  }
  thunk = PyObject_GC_New(_Py_StaticThunk, &_PyType_StaticThunk);
  if (thunk == NULL) {
    return NULL;
  }

  PyObject* func_name = _PyClassLoader_GetFunctionName(name);
  thunk->thunk_tcs.tcs_rt.rt_name = func_name;
  Py_INCREF(func_name);
  thunk->thunk_cls = type;
  Py_XINCREF(type);
  thunk->thunk_vectorcall = (vectorcallfunc)&thunk_vectorcall;
  thunk->thunk_tcs.tcs_value = NULL;

  _PyClassLoader_UpdateThunk(thunk, original, func);

  thunk->thunk_tcs.tcs_rt.rt_expected =
      (PyTypeObject*)_PyClassLoader_ResolveReturnType(
          original,
          &thunk->thunk_tcs.tcs_rt.rt_optional,
          &thunk->thunk_tcs.tcs_rt.rt_exact,
          &thunk->thunk_flags);

  if (Ci_StrictModule_Check(container)) {
    // Treat functions in modules as static, we don't want to peel off the first
    // argument.
    thunk->thunk_flags |= Ci_FUNC_FLAGS_STATICMETHOD;
  }
  if (thunk->thunk_tcs.tcs_rt.rt_expected == NULL) {
    Py_DECREF(thunk);
    return NULL;
  }
  if (PyDict_SetItem(thunks, name, (PyObject*)thunk)) {
    Py_DECREF(thunk);
    return NULL;
  }
  return thunk;
}

/* UpdateModuleName will be called on any patching of a name in a StrictModule.
 */
int _PyClassLoader_UpdateModuleName(
    Ci_StrictModuleObject* mod,
    PyObject* name,
    PyObject* new_value) {
  if (mod->static_thunks != NULL) {
    _Py_StaticThunk* thunk =
        (_Py_StaticThunk*)PyDict_GetItem(mod->static_thunks, name);
    if (thunk != NULL) {
      PyObject* previous = PyDict_GetItem(mod->originals, name);
      _PyClassLoader_UpdateThunk(thunk, previous, new_value);
    }
  }
  return 0;
}
