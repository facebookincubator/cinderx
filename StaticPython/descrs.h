// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject _PyTypedDescriptor_Type;
extern PyTypeObject _PyTypedDescriptorWithDefaultValue_Type;

typedef struct {
  PyObject_HEAD
  PyObject* td_name;
  PyObject* td_type; /* tuple type reference or type object once resolved */
  PyObject* td_default; /* the default value to return from the get if offset is
                           null */
  Py_ssize_t td_offset;
  int td_optional;
  int td_exact;
} _PyTypedDescriptorWithDefaultValue;

typedef struct {
  PyObject_HEAD
  PyObject* td_name;
  PyObject* td_type; /* tuple type reference or type object once resolved */
  Py_ssize_t td_offset;
  int td_optional;
  int td_exact;
} _PyTypedDescriptor;

PyObject*
_PyTypedDescriptor_New(PyObject* name, PyObject* type, Py_ssize_t offset);

PyObject* _PyTypedDescriptorWithDefaultValue_New(
    PyObject* name,
    PyObject* type,
    Py_ssize_t offset,
    PyObject* default_value);

#ifdef __cplusplus
}
#endif
