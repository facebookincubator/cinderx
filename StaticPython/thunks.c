/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/thunks.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"

#include <stddef.h>

static PyObject*
thunk_call(_Py_StaticThunk* thunk, PyObject* args, PyObject* kwds) {
  PyErr_SetString(PyExc_RuntimeError, "thunk_call shouldn't be invokable");
  return NULL;
}

static void _PyClassLoader_MethodThunk_dealloc(_PyClassLoader_MethodThunk* op) {
  _PyClassLoader_FreeThunkSignature(op->mt_sig);
  Py_TYPE(op)->tp_free((PyObject*)op);
}

PyTypeObject _PyType_MethodThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable_method_thunk",
    sizeof(_PyClassLoader_MethodThunk),
    .tp_dealloc = (destructor)_PyClassLoader_MethodThunk_dealloc,
    .tp_flags =
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_MethodThunk, mt_call),
    .tp_call = (ternaryfunc)thunk_call,
    .tp_free = PyObject_Free,
};

static int cachedpropthunktraverse(
    _Py_CachedPropertyThunk* op,
    visitproc visit,
    void* arg) {
  visit(op->propthunk_target, arg);
  return 0;
}

static int cachedpropthunkclear(_Py_CachedPropertyThunk* op) {
  Py_CLEAR(op->propthunk_target);
  return 0;
}

static void cachedpropthunkdealloc(_Py_CachedPropertyThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->propthunk_target);
  PyObject_GC_Del((PyObject*)op);
}

static PyObject* cachedpropthunk_get(
    _Py_CachedPropertyThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_SetString(
        CiExc_StaticTypeError, "cached property get expected 1 argument");
    return NULL;
  }

  descrgetfunc f = PyCachedPropertyWithDescr_Type.tp_descr_get;

  PyObject* res =
      f(thunk->propthunk_target, args[0], (PyObject*)Py_TYPE(args[0]));
  return res;
}

PyTypeObject _PyType_CachedPropertyThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "cachedproperty_thunk",
    sizeof(_Py_CachedPropertyThunk),
    .tp_dealloc = (destructor)cachedpropthunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)cachedpropthunktraverse,
    .tp_clear = (inquiry)cachedpropthunkclear,
    .tp_vectorcall_offset =
        offsetof(_Py_CachedPropertyThunk, propthunk_vectorcall),
    .tp_call = (ternaryfunc)thunk_call,
};

PyObject* _Py_CachedPropertyThunk_GetFunc(PyObject* thunk) {
  assert(Py_TYPE(thunk) == &_PyType_CachedPropertyThunk);
  _Py_CachedPropertyThunk* t = (_Py_CachedPropertyThunk*)thunk;
  PyCachedPropertyDescrObject* descr =
      (PyCachedPropertyDescrObject*)t->propthunk_target;
  return descr->func;
}

_Py_CachedPropertyThunk* _Py_CachedPropertyThunk_New(PyObject* property) {
  _Py_CachedPropertyThunk* thunk =
      PyObject_GC_New(_Py_CachedPropertyThunk, &_PyType_CachedPropertyThunk);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->propthunk_vectorcall = (vectorcallfunc)cachedpropthunk_get;
  thunk->propthunk_target = property;
  Py_INCREF(property);
  return thunk;
}

static int async_cachedpropthunktraverse(
    _Py_AsyncCachedPropertyThunk* op,
    visitproc visit,
    void* arg) {
  visit(op->propthunk_target, arg);
  return 0;
}

static int async_cachedpropthunkclear(_Py_AsyncCachedPropertyThunk* op) {
  Py_CLEAR(op->propthunk_target);
  return 0;
}

static void async_cachedpropthunkdealloc(_Py_AsyncCachedPropertyThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->propthunk_target);
  PyObject_GC_Del((PyObject*)op);
}

static PyObject* async_cachedpropthunk_get(
    _Py_AsyncCachedPropertyThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_SetString(
        CiExc_StaticTypeError, "async cached property get expected 1 argument");
    return NULL;
  }

  descrgetfunc f = PyAsyncCachedPropertyWithDescr_Type.tp_descr_get;

  PyObject* res =
      f(thunk->propthunk_target, args[0], (PyObject*)Py_TYPE(args[0]));
  return res;
}

PyTypeObject _PyType_AsyncCachedPropertyThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "async_cached_property_thunk",
    sizeof(_Py_AsyncCachedPropertyThunk),
    .tp_dealloc = (destructor)async_cachedpropthunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)async_cachedpropthunktraverse,
    .tp_clear = (inquiry)async_cachedpropthunkclear,
    .tp_vectorcall_offset =
        offsetof(_Py_AsyncCachedPropertyThunk, propthunk_vectorcall),
    .tp_call = (ternaryfunc)thunk_call,
};

_Py_AsyncCachedPropertyThunk* _Py_AsyncCachedPropertyThunk_New(
    PyObject* property) {
  _Py_AsyncCachedPropertyThunk* thunk = PyObject_GC_New(
      _Py_AsyncCachedPropertyThunk, &_PyType_AsyncCachedPropertyThunk);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->propthunk_vectorcall = (vectorcallfunc)async_cachedpropthunk_get;
  thunk->propthunk_target = property;
  Py_INCREF(property);
  return thunk;
}

PyObject* _Py_AsyncCachedPropertyThunk_GetFunc(PyObject* thunk) {
  assert(Py_TYPE(thunk) == &_PyType_AsyncCachedPropertyThunk);
  _Py_AsyncCachedPropertyThunk* t = (_Py_AsyncCachedPropertyThunk*)thunk;
  PyAsyncCachedPropertyDescrObject* descr =
      (PyAsyncCachedPropertyDescrObject*)t->propthunk_target;
  return descr->func;
}

static int thunktraverse(_Py_StaticThunk* op, visitproc visit, void* arg) {
  Py_VISIT(op->thunk_tcs.tcs_rt.rt_expected);
  Py_VISIT(op->thunk_tcs.tcs_rt.rt_name);
  Py_VISIT(op->thunk_tcs.tcs_value);
  Py_VISIT((PyObject*)op->thunk_cls);
  return 0;
}

static int thunkclear(_Py_StaticThunk* op) {
  Py_CLEAR(op->thunk_tcs.tcs_rt.rt_expected);
  Py_CLEAR(op->thunk_tcs.tcs_rt.rt_name);
  Py_CLEAR(op->thunk_tcs.tcs_value);
  Py_CLEAR(op->thunk_cls);
  return 0;
}

static void thunkdealloc(_Py_StaticThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->thunk_tcs.tcs_rt.rt_expected);
  Py_XDECREF(op->thunk_tcs.tcs_rt.rt_name);
  Py_XDECREF(op->thunk_tcs.tcs_value);
  Py_XDECREF(op->thunk_cls);
  PyObject_GC_Del((PyObject*)op);
}

PyTypeObject _PyType_StaticThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "static_thunk",
    sizeof(_Py_StaticThunk),
    .tp_dealloc = (destructor)thunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)thunktraverse,
    .tp_clear = (inquiry)thunkclear,
    .tp_vectorcall_offset = offsetof(_Py_StaticThunk, thunk_vectorcall),
    .tp_call = (ternaryfunc)thunk_call,
};

static PyObject*
thunk_call(_Py_StaticThunk* thunk, PyObject* args, PyObject* kwds);

static int
propthunktraverse(_Py_PropertyThunk* op, visitproc visit, void* arg) {
  visit(op->propthunk_target, arg);
  return 0;
}

static int propthunkclear(_Py_PropertyThunk* op) {
  // rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_CLEAR(op->propthunk_target);
  return 0;
}

static void propthunkdealloc(_Py_PropertyThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->propthunk_target);
  PyObject_GC_Del((PyObject*)op);
}

static PyObject* propthunk_get(
    _Py_PropertyThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_SetString(CiExc_StaticTypeError, "property get expected 1 argument");
    return NULL;
  }

  descrgetfunc f = Py_TYPE(thunk->propthunk_target)->tp_descr_get;
  if (f == NULL) {
    Py_INCREF(thunk->propthunk_target);
    return thunk->propthunk_target;
  }

  PyObject* res =
      f(thunk->propthunk_target, args[0], (PyObject*)(Py_TYPE(args[0])));
  return res;
}

static PyObject* propthunk_set(
    _Py_PropertyThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 2) {
    PyErr_SetString(CiExc_StaticTypeError, "property set expected 2 arguments");
    return NULL;
  }

  descrsetfunc f = Py_TYPE(thunk->propthunk_target)->tp_descr_set;
  if (f == NULL) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "'%s' doesn't support __set__",
        Py_TYPE(thunk->propthunk_target)->tp_name);
    return NULL;
  }
  if (f(thunk->propthunk_target, args[0], args[1])) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject* propthunk_del(
    _Py_PropertyThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "property del expected 1 argument, got %zu instead",
        nargs);
    return NULL;
  }

  descrsetfunc f = Py_TYPE(thunk->propthunk_target)->tp_descr_set;
  if (f == NULL) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "'%s' doesn't support __del__",
        Py_TYPE(thunk->propthunk_target)->tp_name);
    return NULL;
  }
  if (f(thunk->propthunk_target, args[0], NULL)) {
    return NULL;
  }
  Py_RETURN_NONE;
}

PyTypeObject _PyType_PropertyThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "property_thunk",
    sizeof(_Py_PropertyThunk),
    .tp_dealloc = (destructor)propthunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)propthunktraverse,
    .tp_clear = (inquiry)propthunkclear,
    .tp_vectorcall_offset = offsetof(_Py_PropertyThunk, propthunk_vectorcall),
    .tp_call = (ternaryfunc)thunk_call,
};

PyObject* _PyClassLoader_PropertyThunkGet_New(PyObject* property) {
  _Py_PropertyThunk* thunk =
      PyObject_GC_New(_Py_PropertyThunk, &_PyType_PropertyThunk);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->propthunk_vectorcall = (vectorcallfunc)propthunk_get;
  thunk->propthunk_target = property;
  Py_INCREF(property);
  return (PyObject*)thunk;
}

PyObject* _PyClassLoader_PropertyThunkSet_New(PyObject* property) {
  _Py_PropertyThunk* thunk =
      PyObject_GC_New(_Py_PropertyThunk, &_PyType_PropertyThunk);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->propthunk_vectorcall = (vectorcallfunc)propthunk_set;
  thunk->propthunk_target = property;
  Py_INCREF(property);
  return (PyObject*)thunk;
}

PyObject* _PyClassLoader_PropertyThunkDel_New(PyObject* property) {
  _Py_PropertyThunk* thunk =
      PyObject_GC_New(_Py_PropertyThunk, &_PyType_PropertyThunk);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->propthunk_vectorcall = (vectorcallfunc)propthunk_del;
  thunk->propthunk_target = property;
  Py_INCREF(property);
  return (PyObject*)thunk;
}

static int typed_descriptor_thunk_traverse(
    _Py_TypedDescriptorThunk* op,
    visitproc visit,
    void* arg) {
  visit(op->typed_descriptor_thunk_target, arg);
  return 0;
}

static int typed_descriptor_thunk_clear(_Py_TypedDescriptorThunk* op) {
  Py_CLEAR(op->typed_descriptor_thunk_target);
  return 0;
}

static void typed_descriptor_thunk_dealloc(_Py_TypedDescriptorThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->typed_descriptor_thunk_target);
  PyObject_GC_Del((PyObject*)op);
}

static PyObject* typed_descriptor_thunk_get(
    _Py_TypedDescriptorThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_SetString(
        CiExc_StaticTypeError, "typed descriptor get expected 1 argument");
    return NULL;
  }
  descrgetfunc f = _PyTypedDescriptorWithDefaultValue_Type.tp_descr_get;
  return f(
      thunk->typed_descriptor_thunk_target,
      args[0],
      (PyObject*)Py_TYPE(args[0]));
}

static PyObject* typed_descriptor_thunk_set(
    _Py_TypedDescriptorThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 2) {
    PyErr_SetString(
        CiExc_StaticTypeError, "typed descriptor set expected 2 arguments");
    return NULL;
  }

  descrsetfunc f = _PyTypedDescriptorWithDefaultValue_Type.tp_descr_set;

  int res = f(thunk->typed_descriptor_thunk_target, args[0], args[1]);
  if (res != 0) {
    return NULL;
  }
  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject* typed_descriptor_thunk_del(
    _Py_TypedDescriptorThunk* thunk,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  size_t nargs = PyVectorcall_NARGS(nargsf);
  if (nargs != 1) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "typed descriptor del expected 1 argument, got %zu instead",
        nargs);
    return NULL;
  }

  descrsetfunc f = _PyTypedDescriptorWithDefaultValue_Type.tp_descr_set;

  int res = f(thunk->typed_descriptor_thunk_target, args[0], NULL);
  if (res != 0) {
    return NULL;
  }
  Py_INCREF(Py_None);
  return Py_None;
}

PyTypeObject _PyType_TypedDescriptorThunk = {
    PyVarObject_HEAD_INIT(
        &PyType_Type,
        0) "typed_descriptor_with_default_value_thunk",
    sizeof(_Py_TypedDescriptorThunk),
    .tp_dealloc = (destructor)typed_descriptor_thunk_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)typed_descriptor_thunk_traverse,
    .tp_clear = (inquiry)typed_descriptor_thunk_clear,
    .tp_vectorcall_offset =
        offsetof(_Py_TypedDescriptorThunk, typed_descriptor_thunk_vectorcall),
    .tp_call = (ternaryfunc)thunk_call,
};

PyObject* _PyClassLoader_TypedDescriptorThunkGet_New(PyObject* property) {
  _Py_TypedDescriptorThunk* thunk =
      PyObject_GC_New(_Py_TypedDescriptorThunk, &_PyType_TypedDescriptorThunk);
  if (thunk == NULL) {
    return NULL;
  }
  Py_INCREF(property);
  thunk->typed_descriptor_thunk_target = property;
  thunk->typed_descriptor_thunk_vectorcall =
      (vectorcallfunc)typed_descriptor_thunk_get;
  thunk->type = THUNK_GETTER;
  return (PyObject*)thunk;
}

PyObject* _PyClassLoader_TypedDescriptorThunkSet_New(PyObject* property) {
  _Py_TypedDescriptorThunk* thunk =
      PyObject_GC_New(_Py_TypedDescriptorThunk, &_PyType_TypedDescriptorThunk);
  if (thunk == NULL) {
    return NULL;
  }
  Py_INCREF(property);
  thunk->typed_descriptor_thunk_target = property;
  thunk->typed_descriptor_thunk_vectorcall =
      (vectorcallfunc)typed_descriptor_thunk_set;
  thunk->type = THUNK_SETTER;
  return (PyObject*)thunk;
}

PyObject* _PyClassLoader_TypedDescriptorThunkDel_New(PyObject* property) {
  _Py_TypedDescriptorThunk* thunk =
      PyObject_GC_New(_Py_TypedDescriptorThunk, &_PyType_TypedDescriptorThunk);
  if (thunk == NULL) {
    return NULL;
  }
  Py_INCREF(property);
  thunk->typed_descriptor_thunk_target = property;
  thunk->typed_descriptor_thunk_vectorcall =
      (vectorcallfunc)typed_descriptor_thunk_del;
  thunk->type = THUNK_DELETER;
  return (PyObject*)thunk;
}

void _PyClassLoader_UpdateThunk(
    _Py_StaticThunk* thunk,
    PyObject* previous,
    PyObject* new_value) {
  Py_CLEAR(thunk->thunk_tcs.tcs_value);
  if (new_value != NULL) {
    PyObject* unwrapped_new = _PyClassLoader_MaybeUnwrapCallable(new_value);
    if (unwrapped_new != NULL) {
      thunk->thunk_tcs.tcs_value = unwrapped_new;
    } else {
      thunk->thunk_tcs.tcs_value = new_value;
      Py_INCREF(new_value);
    }
  }
  PyObject* funcref;
  if (new_value == previous) {
    funcref = previous;
  } else {
    funcref = (PyObject*)thunk;
  }
  PyObject* unwrapped = _PyClassLoader_MaybeUnwrapCallable(funcref);
  if (unwrapped != NULL) {
    thunk->thunk_funcref = unwrapped;
    Py_DECREF(unwrapped);
  } else {
    thunk->thunk_funcref = funcref;
  }
}
