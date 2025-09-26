/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/static_array.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_modsupport.h"
#endif

/**
 *   Lightweight implementation of Static Arrays.
 */

#define ArrayItemType int64_t

PyType_Spec PyStaticArray_Spec;

static void staticarray_dealloc(PyStaticArrayObject* op) {
  PyObject_GC_UnTrack(op);
  PyTypeObject* type = Py_TYPE(op);
  type->tp_free((PyObject*)op);
  Py_DECREF(type);
}

static PyStaticArrayObject* staticarray_alloc(Py_ssize_t size) {
  PyStaticArrayObject* op =
      PyObject_GC_NewVar(PyStaticArrayObject, PyStaticArray_Type, size);
  return op;
}

static inline void staticarray_zeroinitialize(
    PyStaticArrayObject* sa,
    Py_ssize_t size) {
  memset(sa->ob_item, 0, size * PyStaticArray_Spec.itemsize);
}

static PyObject* staticarray_to_list(PyObject* sa) {
  PyStaticArrayObject* array = (PyStaticArrayObject*)sa;
  PyObject* list = PyList_New(Py_SIZE(sa));

  if (list == NULL) {
    return NULL;
  }

  for (Py_ssize_t i = 0; i < Py_SIZE(sa); i++) {
    ArrayItemType val = array->ob_item[i];
    PyObject* boxed_val = PyLong_FromLong(val);
    if (boxed_val == NULL) {
      Py_DECREF(list);
      return NULL;
    }
    PyList_SET_ITEM(list, i, boxed_val);
  }
  return list;
}

static PyObject* staticarray_repr(PyObject* sa) {
  PyObject* list = staticarray_to_list(sa);
  if (list == NULL) {
    return NULL;
  }
  PyObject* res =
      PyUnicode_FromFormat("staticarray[%d](%R)", Py_SIZE(sa), list);
  Py_DECREF(list);
  return res;
}

static Py_ssize_t staticarray_length(PyStaticArrayObject* a) {
  return Py_SIZE(a);
}

static int staticarray_traverse(PyObject* self, visitproc visit, void* arg) {
  return 0;
}

static PyObject* staticarray_concat(
    PyStaticArrayObject* first,
    PyObject* other) {
  Py_ssize_t size;
  PyStaticArrayObject* np;
  if (!PyStaticArray_CheckExact(other)) {
    PyErr_Format(
        PyExc_TypeError,
        "can only append staticarray (not \"%.200s\") to staticarray",
        Py_TYPE(other)->tp_name);
    return NULL;
  }
  PyStaticArrayObject* second = (PyStaticArrayObject*)other;
  if (Py_SIZE(first) > PY_SSIZE_T_MAX - Py_SIZE(second)) {
    return PyErr_NoMemory();
  }
  size = Py_SIZE(first) + Py_SIZE(second);
  np = staticarray_alloc(size);
  if (np == NULL) {
    return NULL;
  }
  Py_ssize_t itemsize = PyStaticArray_Spec.itemsize;
  if (Py_SIZE(first) > 0) {
    memcpy(np->ob_item, first->ob_item, Py_SIZE(first) * itemsize);
  }
  if (Py_SIZE(second) > 0) {
    memcpy(
        np->ob_item + Py_SIZE(first),
        second->ob_item,
        Py_SIZE(second) * itemsize);
  }
  return (PyObject*)np;
}

static PyObject* staticarray_repeat(PyStaticArrayObject* array, Py_ssize_t n) {
  Py_ssize_t size;
  PyStaticArrayObject* np;
  if (n < 0) {
    return (PyObject*)staticarray_alloc(0);
  }
  if ((Py_SIZE(array) != 0) && (n > PY_SSIZE_T_MAX / Py_SIZE(array))) {
    return PyErr_NoMemory();
  }
  size = Py_SIZE(array) * n;
  np = (PyStaticArrayObject*)staticarray_alloc(size);
  if (np == NULL) {
    return NULL;
  }
  if (size == 0) {
    return (PyObject*)np;
  }

  Py_ssize_t oldsize = Py_SIZE(array);
  Py_ssize_t newsize = oldsize * n;
  Py_ssize_t itemsize = PyStaticArray_Spec.itemsize;

  Py_ssize_t done = oldsize;
  memcpy(np->ob_item, array->ob_item, oldsize * itemsize);
  while (done < newsize) {
    Py_ssize_t ncopy = (done <= newsize - done) ? done : newsize - done;
    memcpy(np->ob_item + done, np->ob_item, ncopy * itemsize);
    done += ncopy;
  }

  return (PyObject*)np;
}

static PyObject* staticarray_getitem(
    PyStaticArrayObject* array,
    Py_ssize_t index) {
  index = index < 0 ? index + Py_SIZE(array) : index;
  if (index < 0 || index >= Py_SIZE(array)) {
    PyErr_SetString(PyExc_IndexError, "array index out of range");
    return NULL;
  }
  assert(PyStaticArray_Spec.itemsize == sizeof(long));
  return PyLong_FromLong(array->ob_item[index]);
}

static int staticarray_setitem(
    PyStaticArrayObject* array,
    Py_ssize_t index,
    PyObject* value) {
  index = index < 0 ? index + Py_SIZE(array) : index;
  if (index < 0 || index >= Py_SIZE(array)) {
    PyErr_SetString(PyExc_IndexError, "array index out of range");
    return -1;
  }
  assert(PyStaticArray_Spec.itemsize == sizeof(long));
  ArrayItemType val = PyLong_AsLong(value);
  if (val == -1 && PyErr_Occurred()) {
    return -1;
  }
  array->ob_item[index] = val;
  return 0;
}

PyObject* staticarray___class_getitem__(PyObject* origin, PyObject* args) {
  Py_INCREF(origin);
  return origin;
}

PyObject* staticarray_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
  if (!_PyArg_NoKeywords("staticarray", kwds)) {
    return NULL;
  }

  Py_ssize_t nargs = PyTuple_GET_SIZE(args);
  if (!_PyArg_CheckPositional("staticarray", nargs, 1, 1)) {
    return NULL;
  }

  PyObject* length = PyTuple_GET_ITEM(args, 0);
  Py_ssize_t size = PyLong_AsSize_t(length);
  if (size == -1 && PyErr_Occurred()) {
    return NULL;
  }
  PyStaticArrayObject* new = (PyStaticArrayObject*)type->tp_alloc(type, size);
  staticarray_zeroinitialize(new, size);
  return (PyObject*)new;
}

static PyMethodDef staticarray_methods[] = {
    {"__class_getitem__",
     (PyCFunction)staticarray___class_getitem__,
     METH_O | METH_CLASS,
     PyDoc_STR("")},
    {NULL, NULL} /* sentinel */
};

PyType_Slot staticarray_slots[] = {
    {Py_tp_dealloc, staticarray_dealloc},
    {Py_tp_free, PyObject_GC_Del},
    {Py_tp_repr, staticarray_repr},
    {Py_tp_methods, staticarray_methods},
    {Py_tp_new, staticarray_new},
    {Py_sq_length, staticarray_length},
    {Py_sq_concat, staticarray_concat},
    {Py_sq_repeat, staticarray_repeat},
    {Py_sq_item, staticarray_getitem},
    {Py_sq_ass_item, staticarray_setitem},
    {Py_tp_traverse, staticarray_traverse},
    {}};

PyType_Spec PyStaticArray_Spec = {
    .name = "__static__.staticarray",
    .basicsize = sizeof(PyStaticArrayObject),
    .itemsize = sizeof(ArrayItemType),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = staticarray_slots,
};

PyTypeObject* PyStaticArray_Type;

/** StaticArray internal C-API **/

int _Ci_StaticArray_Set(PyObject* array, Py_ssize_t index, PyObject* value) {
  PyStaticArrayObject* sa = (PyStaticArrayObject*)array;
  return staticarray_setitem(sa, index, value);
}

PyObject* _Ci_StaticArray_Get(PyObject* array, Py_ssize_t index) {
  PyStaticArrayObject* sa = (PyStaticArrayObject*)array;
  return staticarray_getitem(sa, index);
}
