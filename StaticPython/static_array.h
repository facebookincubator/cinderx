/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#ifndef Ci_STATIC_ARRAY_H
#define Ci_STATIC_ARRAY_H

#include "Python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern PyType_Spec PyStaticArray_Spec;
extern PyTypeObject* PyStaticArray_Type;
#define PyStaticArray_CheckExact(op) Py_IS_TYPE(op, PyStaticArray_Type)

typedef struct {
  PyObject_VAR_HEAD
      /* ob_item contains space for 'ob_size' elements. */
      int64_t ob_item[1];
} PyStaticArrayObject;

int _Ci_StaticArray_Set(PyObject* array, Py_ssize_t index, PyObject* value);
PyObject* _Ci_StaticArray_Get(PyObject* array, Py_ssize_t index);

#ifdef __cplusplus
}
#endif

#endif
