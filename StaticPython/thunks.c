/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/thunks.h"

#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"

#include <stddef.h>

static PyObject* thunk_call(PyObject* thunk, PyObject* args, PyObject* kwds) {
  PyErr_SetString(PyExc_RuntimeError, "thunk_call shouldn't be invokable");
  return NULL;
}

static void vtableinitthunk_dealloc(_PyClassLoader_VTableInitThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->vti_name);
  Py_XDECREF(op->vti_type);
  PyObject_GC_Del((PyObject*)op);
}

static int vtableinitthunk_traverse(
    _PyClassLoader_VTableInitThunk* op,
    visitproc visit,
    void* arg) {
  Py_VISIT(op->vti_type);
  return 0;
}

static int vtableinitthunk_clear(_PyClassLoader_VTableInitThunk* op) {
  Py_CLEAR(op->vti_name);
  Py_CLEAR(op->vti_type);
  return 0;
}

PyTypeObject _PyClassLoader_VTableInitThunk_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable_init_thunk",
    sizeof(_PyClassLoader_VTableInitThunk),
    .tp_dealloc = (destructor)vtableinitthunk_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)vtableinitthunk_traverse,
    .tp_clear = (inquiry)vtableinitthunk_clear,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_VTableInitThunk, vti_call),
    .tp_call = (ternaryfunc)thunk_call,
};

PyObject* _PyClassLoader_VTableInitThunk_New(
    PyObject* name,
    PyTypeObject* type,
    vectorcallfunc call) {
  _PyClassLoader_VTableInitThunk* thunk = PyObject_GC_New(
      _PyClassLoader_VTableInitThunk, &_PyClassLoader_VTableInitThunk_Type);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->vti_name = Py_NewRef(name);
  thunk->vti_type = (PyTypeObject*)Py_NewRef(type);
  thunk->vti_call = call;
  PyObject_GC_Track(thunk);
  return (PyObject*)thunk;
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

PyObject* _PyClassLoader_PropertyThunk_GetProperty(PyObject* thunk) {
  assert(Py_TYPE(thunk) == &_PyType_PropertyThunk);
  _Py_PropertyThunk* t = (_Py_PropertyThunk*)thunk;
  return t->propthunk_target;
}

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

PropertyThunkKind _PyClassLoader_PropertyThunk_Kind(PyObject* property) {
  assert(Py_TYPE(property) == &_PyType_PropertyThunk);
  _Py_PropertyThunk* thunk = (_Py_PropertyThunk*)property;
  if (thunk->propthunk_vectorcall == (vectorcallfunc)propthunk_get) {
    return THUNK_GETTER;
  } else if (thunk->propthunk_vectorcall == (vectorcallfunc)propthunk_set) {
    return THUNK_SETTER;
  }
  return THUNK_DELETER;
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

static int rettype_check_clear(_PyClassLoader_RetTypeInfo* op) {
  Py_CLEAR(op->rt_expected);
  Py_CLEAR(op->rt_name);
  return 0;
}

static int rettype_check_traverse(
    _PyClassLoader_RetTypeInfo* op,
    visitproc visit,
    void* arg) {
  Py_VISIT(op->rt_expected);
  return 0;
}

static int _PyClassLoader_TypeCheckThunk_traverse(
    _PyClassLoader_TypeCheckThunk* op,
    visitproc visit,
    void* arg) {
  rettype_check_traverse((_PyClassLoader_RetTypeInfo*)op, visit, arg);
  Py_VISIT(op->tcs_value);
  return 0;
}

static int _PyClassLoader_TypeCheckThunk_clear(
    _PyClassLoader_TypeCheckThunk* op) {
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_CLEAR(op->tcs_value);
  return 0;
}

static void _PyClassLoader_TypeCheckThunk_dealloc(
    _PyClassLoader_TypeCheckThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_XDECREF(op->tcs_value);
  _PyClassLoader_FreeThunkSignature(op->tcs_rt.rt_base.mt_sig);
  PyObject_GC_Del((PyObject*)op);
}

PyTypeObject _PyType_TypeCheckThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable_state_obj",
    sizeof(_PyClassLoader_TypeCheckThunk),
    .tp_base = &_PyType_MethodThunk,
    .tp_dealloc = (destructor)_PyClassLoader_TypeCheckThunk_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)_PyClassLoader_TypeCheckThunk_traverse,
    .tp_clear = (inquiry)_PyClassLoader_TypeCheckThunk_clear,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_MethodThunk, mt_call),
    .tp_call = thunk_call,
    .tp_free = PyObject_GC_Del,
};

PyObject* _PyClassLoader_TypeCheckThunk_New(
    PyObject* value,
    PyObject* name,
    PyTypeObject* ret_type,
    int optional,
    int exact,
    _PyClassLoader_ThunkSignature* sig) {
  _PyClassLoader_TypeCheckThunk* thunk =
      PyObject_GC_New(_PyClassLoader_TypeCheckThunk, &_PyType_TypeCheckThunk);
  if (thunk == NULL) {
    return NULL;
  }

  thunk->tcs_value = value;
  Py_INCREF(value);
  thunk->tcs_rt.rt_name = name;
  Py_INCREF(name);
  thunk->tcs_rt.rt_expected = (PyTypeObject*)ret_type;
  Py_INCREF(ret_type);
  thunk->tcs_rt.rt_optional = optional;
  thunk->tcs_rt.rt_exact = exact;
  thunk->tcs_rt.rt_base.mt_sig = sig;
  thunk->tcs_rt.rt_base.mt_call = NULL;
  PyObject_GC_Track(thunk);
  return (PyObject*)thunk;
}

static int lazyfuncinit_thunk_traverse(
    _PyClassLoader_LazyFuncJitThunk* op,
    visitproc visit,
    void* arg) {
  Py_VISIT(op->lf_vtable);
  Py_VISIT(op->lf_func);
  return 0;
}

static int lazyfuncinit_thunk_clear(_PyClassLoader_LazyFuncJitThunk* op) {
  Py_CLEAR(op->lf_vtable);
  Py_CLEAR(op->lf_func);
  return 0;
}

static void lazyfuncinit_thunk_dealloc(_PyClassLoader_LazyFuncJitThunk* op) {
  Py_XDECREF(op->lf_vtable);
  Py_XDECREF(op->lf_func);
  PyObject_GC_UnTrack(op);
  _PyClassLoader_MethodThunk_dealloc((_PyClassLoader_MethodThunk*)op);
}

PyTypeObject _PyClassLoader_LazyFuncJitThunk_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "lazyfuncinit_thunk",
    sizeof(_PyClassLoader_LazyFuncJitThunk),
    .tp_base = &_PyType_MethodThunk,
    .tp_dealloc = (destructor)lazyfuncinit_thunk_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)lazyfuncinit_thunk_traverse,
    .tp_clear = (inquiry)lazyfuncinit_thunk_clear,
    .tp_call = (ternaryfunc)thunk_call,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_MethodThunk, mt_call),
    .tp_free = PyObject_GC_Del,
};

PyObject* _PyClassLoader_LazyFuncJitThunk_New(
    PyObject* vtable,
    Py_ssize_t slot,
    PyFunctionObject* original,
    _PyClassLoader_ThunkSignature* sig,
    vectorcallfunc call) {
  _PyClassLoader_LazyFuncJitThunk* thunk = PyObject_GC_New(
      _PyClassLoader_LazyFuncJitThunk, &_PyClassLoader_LazyFuncJitThunk_Type);
  if (thunk == NULL) {
    return NULL;
  }
  Py_INCREF(vtable);
  Py_INCREF(original);
  thunk->lf_vtable = vtable;
  thunk->lf_slot = slot;
  thunk->lf_base.mt_sig = sig;
  thunk->lf_func = original;
  thunk->lf_base.mt_call = call;
  PyObject_GC_Track(thunk);
  return (PyObject*)thunk;
}

static int staticmethodthunktraverse(
    _PyClassLoader_StaticMethodThunk* op,
    visitproc visit,
    void* arg) {
  Py_VISIT(op->smt_func);
  return 0;
}

static int staticmethodthunkclear(_PyClassLoader_StaticMethodThunk* op) {
  Py_CLEAR(op->smt_func);
  return 0;
}

static void staticmethodthunkdealloc(_PyClassLoader_StaticMethodThunk* op) {
  Py_XDECREF(op->smt_func);
  PyObject_GC_UnTrack(op);
  _PyClassLoader_MethodThunk_dealloc(&op->smt_base);
}

PyTypeObject _PyClassLoader_StaticMethodThunk_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "staticmethod_thunk",
    sizeof(_PyClassLoader_StaticMethodThunk),
    .tp_base = &_PyType_MethodThunk,
    .tp_dealloc = (destructor)staticmethodthunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)staticmethodthunktraverse,
    .tp_clear = (inquiry)staticmethodthunkclear,
    .tp_call = (ternaryfunc)thunk_call,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_MethodThunk, mt_call),
    .tp_free = PyObject_GC_Del,
};

// Steals the lifetime of the signature
PyObject* _PyClassLoader_StaticMethodThunk_New(
    PyObject* func,
    _PyClassLoader_ThunkSignature* sig,
    vectorcallfunc call) {
  _PyClassLoader_StaticMethodThunk* thunk = PyObject_GC_New(
      _PyClassLoader_StaticMethodThunk, &_PyClassLoader_StaticMethodThunk_Type);
  if (thunk == NULL) {
    _PyClassLoader_FreeThunkSignature(sig);
    return NULL;
  }
  thunk->smt_base.mt_sig = sig;
  thunk->smt_func = Py_NewRef(func);
  thunk->smt_base.mt_call = call;
  PyObject_GC_Track(thunk);
  return (PyObject*)thunk;
}

static int classmethodthunktraverse(
    _PyClassLoader_ClassMethodThunk* op,
    visitproc visit,
    void* arg) {
  Py_VISIT(op->cmt_decl_type);
  Py_VISIT(op->cmt_classmethod);
  return 0;
}

static int classmethodthunkclear(_PyClassLoader_ClassMethodThunk* op) {
  Py_CLEAR(op->cmt_decl_type);
  Py_CLEAR(op->cmt_classmethod);
  return 0;
}

static void classmethodthunkdealloc(_PyClassLoader_ClassMethodThunk* op) {
  Py_XDECREF(op->cmt_decl_type);
  Py_XDECREF(op->cmt_classmethod);
  PyObject_GC_UnTrack(op);
  _PyClassLoader_MethodThunk_dealloc(&op->cmt_base);
}

PyTypeObject _PyClassLoader_ClassMethodThunk_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "classmethod_thunk",
    sizeof(_PyClassLoader_ClassMethodThunk),
    .tp_base = &_PyType_MethodThunk,
    .tp_dealloc = (destructor)classmethodthunkdealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)classmethodthunktraverse,
    .tp_clear = (inquiry)classmethodthunkclear,
    .tp_call = (ternaryfunc)thunk_call,
    .tp_vectorcall_offset = offsetof(_PyClassLoader_MethodThunk, mt_call),
    .tp_free = PyObject_GC_Del,
};

PyObject* _PyClassLoader_ClassMethodThunk_New(
    PyObject* classmethod,
    _PyClassLoader_ThunkSignature* sig,
    PyTypeObject* decl_type,
    vectorcallfunc call) {
  _PyClassLoader_ClassMethodThunk* thunk = PyObject_GC_New(
      _PyClassLoader_ClassMethodThunk, &_PyClassLoader_ClassMethodThunk_Type);
  if (thunk == NULL) {
    return NULL;
  }
  thunk->cmt_classmethod = Py_NewRef(classmethod);
  thunk->cmt_decl_type = (PyTypeObject*)Py_NewRef(decl_type);

  thunk->cmt_base.mt_sig = sig;
  thunk->cmt_base.mt_call = call;
  PyObject_GC_Track(thunk);
  return (PyObject*)thunk;
}
