/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "cinderx/StaticPython/objectkey.h"

static PyObject*
objectkey_richcmp(_Ci_ObjectKey* self, PyObject* other, int op) {
  if (op != Py_EQ && op != Py_NE) {
    Py_RETURN_NOTIMPLEMENTED;
  }
  if (op == Py_EQ) {
    if (Py_TYPE(other) == &_Ci_ObjectKeyType) {
      if (self->obj == ((_Ci_ObjectKey*)other)->obj) {
        Py_RETURN_TRUE;
      }
    } else if (self->obj == other) {
      Py_RETURN_TRUE;
    }
  } else {
    if (Py_TYPE(other) == &_Ci_ObjectKeyType) {
      if (self->obj != ((_Ci_ObjectKey*)other)->obj) {
        Py_RETURN_TRUE;
      }
    } else if (self->obj != other) {
      Py_RETURN_TRUE;
    }
  }
  Py_RETURN_FALSE;
}

static Py_hash_t objectkey_hash(_Ci_ObjectKey* self) {
  return (Py_hash_t)self->obj;
}

PyTypeObject _Ci_ObjectKeyType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0).tp_name = "object_key",
    .tp_basicsize = sizeof(_Ci_ObjectKey),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_richcompare = (richcmpfunc)objectkey_richcmp,
    .tp_hash = (hashfunc)objectkey_hash,
};

PyObject* _Ci_ObjectKey_New(PyObject* obj) {
  _Ci_ObjectKey* res = PyObject_New(_Ci_ObjectKey, &_Ci_ObjectKeyType);
  if (res != NULL) {
    res->obj = obj; // borrowed
  }
  return (PyObject*)res;
}
