/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/descrs.h"

#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/type.h"

static void typed_descriptor_dealloc(_PyTypedDescriptor* self) {
  PyObject_GC_UnTrack(self);
  Py_XDECREF(self->td_name);
  Py_XDECREF(self->td_type);
  Py_TYPE(self)->tp_free(self);
}

static int typed_descriptor_traverse(
    _PyTypedDescriptor* self,
    visitproc visit,
    void* arg) {
  Py_VISIT(self->td_type);
  return 0;
}

static int typed_descriptor_clear(_PyTypedDescriptor* self) {
  Py_CLEAR(self->td_type);
  return 0;
}

static PyObject*
typed_descriptor_get(PyObject* self, PyObject* obj, PyObject* cls) {
  _PyTypedDescriptor* td = (_PyTypedDescriptor*)self;

  if (obj == NULL) {
    Py_INCREF(self);
    return self;
  }

  PyObject* res = *(PyObject**)(((char*)obj) + td->td_offset);
  if (res == NULL) {
    PyErr_Format(
        PyExc_AttributeError,
        "'%s' object has no attribute '%U'",
        Py_TYPE(obj)->tp_name,
        td->td_name);
    return NULL;
  }
  Py_INCREF(res);
  return res;
}

static int
typed_descriptor_set(PyObject* self, PyObject* obj, PyObject* value) {
  _PyTypedDescriptor* td = (_PyTypedDescriptor*)self;
  if (PyTuple_CheckExact(td->td_type)) {
    PyTypeObject* type = _PyClassLoader_ResolveType(
        td->td_type, &td->td_optional, &td->td_exact);
    if (type == NULL) {
      assert(PyErr_Occurred());
      if (value == Py_None && td->td_optional) {
        /* allow None assignment to optional values before the class is
         * loaded */
        PyErr_Clear();
        PyObject** addr = (PyObject**)(((char*)obj) + td->td_offset);
        PyObject* prev = *addr;
        *addr = value;
        Py_INCREF(value);
        Py_XDECREF(prev);
        return 0;
      }
      return -1;
    }
    Py_DECREF(td->td_type);
    td->td_type = (PyObject*)type;
  }

  if (value == NULL ||
      _PyObject_TypeCheckOptional(
          value, (PyTypeObject*)td->td_type, td->td_optional, td->td_exact)) {
    PyObject** addr = (PyObject**)(((char*)obj) + td->td_offset);
    PyObject* prev = *addr;
    *addr = value;
    Py_XINCREF(value);
    Py_XDECREF(prev);
    return 0;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s' for attribute '%U'",
      ((PyTypeObject*)td->td_type)->tp_name,
      Py_TYPE(value)->tp_name,
      td->td_name);

  return -1;
}

PyTypeObject _PyTypedDescriptor_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0).tp_name = "typed_descriptor",
    .tp_basicsize = sizeof(_PyTypedDescriptor),
    .tp_dealloc = (destructor)typed_descriptor_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
    .tp_traverse = (traverseproc)typed_descriptor_traverse,
    .tp_clear = (inquiry)typed_descriptor_clear,
    .tp_descr_get = typed_descriptor_get,
    .tp_descr_set = typed_descriptor_set,
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_GC_Del,
};

PyObject*
_PyTypedDescriptor_New(PyObject* name, PyObject* type, Py_ssize_t offset) {
  if (PyType_Ready(&_PyTypedDescriptor_Type) < 0) {
    return NULL;
  }
  _PyTypedDescriptor* res =
      PyObject_GC_New(_PyTypedDescriptor, &_PyTypedDescriptor_Type);
  if (res == NULL) {
    return NULL;
  }

  res->td_name = name;
  res->td_type = type;
  res->td_offset = offset;
  res->td_optional = 0;
  res->td_exact = 0;
  Py_INCREF(name);
  Py_INCREF(type);
  PyObject_GC_Track(res);
  return (PyObject*)res;
}

static void typed_descriptor_with_default_value_dealloc(
    _PyTypedDescriptorWithDefaultValue* self) {
  PyObject_GC_UnTrack(self);
  Py_XDECREF(self->td_name);
  Py_XDECREF(self->td_type);
  Py_XDECREF(self->td_default);
  Py_TYPE(self)->tp_free(self);
}

static int typed_descriptor_with_default_value_traverse(
    _PyTypedDescriptorWithDefaultValue* self,
    visitproc visit,
    void* arg) {
  Py_VISIT(self->td_name);
  Py_VISIT(self->td_type);
  Py_VISIT(self->td_default);
  return 0;
}

static int typed_descriptor_with_default_value_clear(
    _PyTypedDescriptorWithDefaultValue* self) {
  Py_CLEAR(self->td_name);
  Py_CLEAR(self->td_type);
  Py_CLEAR(self->td_default);
  return 0;
}

static PyObject* typed_descriptor_with_default_value_get(
    PyObject* self,
    PyObject* obj,
    PyObject* cls) {
  _PyTypedDescriptorWithDefaultValue* td =
      (_PyTypedDescriptorWithDefaultValue*)self;
  if (obj == NULL) {
    /* Since we don't have any APIs supporting the modification of the default,
       it should always be set. */
    assert(td->td_default != NULL);
    Py_INCREF(td->td_default);
    return td->td_default;
  }

  PyObject* res = *(PyObject**)(((char*)obj) + td->td_offset);
  if (res == NULL) {
    res = td->td_default;
  }
  if (res == NULL) {
    PyErr_Format(
        PyExc_AttributeError,
        "'%s' object has no attribute '%U'",
        ((PyTypeObject*)cls)->tp_name,
        td->td_name);
  }
  Py_XINCREF(res);
  return res;
}

static int typed_descriptor_with_default_value_set(
    PyObject* self,
    PyObject* obj,
    PyObject* value) {
  _PyTypedDescriptorWithDefaultValue* td =
      (_PyTypedDescriptorWithDefaultValue*)self;
  if (PyTuple_CheckExact(td->td_type)) {
    PyTypeObject* type = _PyClassLoader_ResolveType(
        td->td_type, &td->td_optional, &td->td_exact);
    if (type == NULL) {
      assert(PyErr_Occurred());
      if (value == Py_None && td->td_optional) {
        /* allow None assignment to optional values before the class is
         * loaded */
        PyErr_Clear();
        PyObject** addr = (PyObject**)(((char*)obj) + td->td_offset);
        PyObject* prev = *addr;
        *addr = value;
        Py_XINCREF(value);
        Py_XDECREF(prev);
        return 0;
      }
      return -1;
    }
    Py_DECREF(td->td_type);
    td->td_type = (PyObject*)type;
  }

  if (value == NULL ||
      _PyObject_TypeCheckOptional(
          value, (PyTypeObject*)td->td_type, td->td_optional, td->td_exact)) {
    PyObject** addr = (PyObject**)(((char*)obj) + td->td_offset);
    PyObject* prev = *addr;
    *addr = value;
    Py_XINCREF(value);
    Py_XDECREF(prev);
    return 0;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s' for attribute '%U'",
      ((PyTypeObject*)td->td_type)->tp_name,
      Py_TYPE(value)->tp_name,
      td->td_name);

  return -1;
}

PyTypeObject _PyTypedDescriptorWithDefaultValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0).tp_name =
        "typed_descriptor_with_default_value",
    .tp_basicsize = sizeof(_PyTypedDescriptorWithDefaultValue),
    .tp_dealloc = (destructor)typed_descriptor_with_default_value_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
    .tp_traverse = (traverseproc)typed_descriptor_with_default_value_traverse,
    .tp_clear = (inquiry)typed_descriptor_with_default_value_clear,
    .tp_descr_get = typed_descriptor_with_default_value_get,
    .tp_descr_set = typed_descriptor_with_default_value_set,
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_GC_Del,
};

PyObject* _PyTypedDescriptorWithDefaultValue_New(
    PyObject* name,
    PyObject* type,
    Py_ssize_t offset,
    PyObject* default_value) {
  if (PyType_Ready(&_PyTypedDescriptorWithDefaultValue_Type) < 0) {
    return NULL;
  }
  _PyTypedDescriptorWithDefaultValue* res = PyObject_GC_New(
      _PyTypedDescriptorWithDefaultValue,
      &_PyTypedDescriptorWithDefaultValue_Type);
  if (res == NULL) {
    return NULL;
  }

  res->td_name = name;
  res->td_type = type;
  res->td_offset = offset;

  res->td_optional = 0;
  res->td_exact = 0;
  res->td_default = default_value;
  Py_INCREF(name);
  Py_INCREF(type);
  Py_INCREF(default_value);
  PyObject_GC_Track(res);
  return (PyObject*)res;
}
