/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <cinderx/python.h>

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_modsupport.h"
#endif

#include "cinderx/Common/import.h"
#include "cinderx/StaticPython/generic_type.h"
#include "cinderx/StaticPython/typed_method_def.h"

PyDoc_STRVAR(
    xxclassloader__doc__,
    "xxclassloader contains helpers for testing the class loader\n");

/* We link this module statically for convenience.  If compiled as a shared
   library instead, some compilers don't allow addresses of Python objects
   defined in other libraries to be used in static initializers here.  The
   DEFERRED_ADDRESS macro is used to tag the slots where such addresses
   appear; the module init function must fill in the tagged slots at runtime.
   The argument is for documentation -- the macro ignores it.
*/
#define DEFERRED_ADDRESS(ADDR) 0

/* spamobj -- a generic type*/

typedef struct {
  PyObject_HEAD
  PyObject* state;
  PyObject* str;
  Py_ssize_t val;
  size_t uval;
} spamobject;

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static Py_ssize_t spamobj_error(spamobject* self, Py_ssize_t val) {
  if (val) {
    PyErr_SetString(PyExc_TypeError, "no way!");
    return -1;
  }
  return 0;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static PyObject* spamobj_getstate(spamobject* self) {
  if (self->state) {
    Py_INCREF(self->state);
    return self->state;
  }
  Py_INCREF(Py_None);
  return Py_None;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setstate(spamobject* self, PyObject* state) {
  Py_XDECREF(self->state);
  self->state = state;
  Py_INCREF(state);
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static PyObject* spamobj_setstate_untyped(spamobject* self, PyObject* state) {
  if (!_PyClassLoader_CheckParamType((PyObject*)self, state, 0)) {
    PyErr_SetString(PyExc_TypeError, "bad type");
    return NULL;
  }
  Py_XDECREF(self->state);
  self->state = state;
  Py_INCREF(state);
  Py_RETURN_NONE;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setstate_optional(spamobject* self, PyObject* state) {
  if (state == Py_None) {
    return;
  }
  Py_XDECREF(self->state);
  self->state = state;
  Py_INCREF(state);
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setint(spamobject* self, Py_ssize_t val) {
  self->val = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setint8(spamobject* self, int8_t val) {
  self->val = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setint16(spamobject* self, int16_t val) {
  self->val = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setint32(spamobject* self, int32_t val) {
  self->val = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setuint8(spamobject* self, uint8_t val) {
  self->uval = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setuint16(spamobject* self, uint16_t val) {
  self->uval = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setuint32(spamobject* self, uint32_t val) {
  self->uval = val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setuint64(spamobject* self, uint64_t val) {
  self->uval = val;
}

static Py_ssize_t
// NOLINTNEXTLINE(clang-diagnostic-unused-function)
spamobj_twoargs(spamobject* self, Py_ssize_t x, Py_ssize_t y) {
  return x + y;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static Py_ssize_t spamobj_getint(spamobject* self) {
  return self->val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static int8_t spamobj_getint8(spamobject* self) {
  return self->val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static int16_t spamobj_getint16(spamobject* self) {
  return self->val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static int32_t spamobj_getint32(spamobject* self) {
  return self->val;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static uint8_t spamobj_getuint8(spamobject* self) {
  return self->uval;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static uint16_t spamobj_getuint16(spamobject* self) {
  return self->uval;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static uint32_t spamobj_getuint32(spamobject* self) {
  return self->uval;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static uint64_t spamobj_getuint64(spamobject* self) {
  return self->uval;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static void spamobj_setstr(spamobject* self, PyObject* str) {
  Py_XDECREF(self->str);
  self->str = str;
  Py_INCREF(str);
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
static PyObject* spamobj_getstr(spamobject* self) {
  if (self->str == NULL) {
    Py_RETURN_NONE;
  }
  Py_INCREF(self->str);
  return self->str;
}

static PyMethodDef spamobj_methods[] = {
    {"__class_getitem__",
     (PyCFunction)_PyClassLoader_GtdGetItem,
     METH_VARARGS | METH_CLASS,
     NULL},
    {NULL, NULL},
};

static int spamobj_traverse(spamobject* o, visitproc visit, void* arg) {
  Py_VISIT(o->state);
  Py_VISIT(o->str);
  return 0;
}

static int spamobj_clear(spamobject* o) {
  Py_CLEAR(o->state);
  Py_CLEAR(o->str);
  return 0;
}

static void spamobj_dealloc(spamobject* o) {
  PyObject_GC_UnTrack(o);
  Py_XDECREF(o->state);
  Py_XDECREF(o->str);
  Py_TYPE(o)->tp_free(o);
}

_PyGenericTypeDef spamobj_type = {
    .gtd_type =
        {.ht_type =
             {
                 PyVarObject_HEAD_INIT(NULL, 0) "spamobj[T]",
                 sizeof(spamobject),
                 0,
                 (destructor)spamobj_dealloc, /* tp_dealloc */
                 0, /* tp_vectorcall_offset */
                 0, /* tp_getattr */
                 0, /* tp_setattr */
                 0, /* tp_as_async */
                 0, /* tp_repr */
                 0, /* tp_as_number */
                 0, /* tp_as_sequence */
                 0, /* tp_as_mapping */
                 0, /* tp_hash */
                 0, /* tp_call */
                 0, /* tp_str */
                 0, /* tp_getattro */
                 0, /* tp_setattro */
                 0, /* tp_as_buffer */
                 Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                     Ci_Py_TPFLAGS_GENERIC_TYPE_DEF, /* tp_flags */
                 0, /* tp_doc */
                 (traverseproc)spamobj_traverse, /* tp_traverse */
                 (inquiry)spamobj_clear, /* tp_clear */
                 0, /* tp_richcompare */
                 0, /* tp_weaklistoffset */
                 0, /* tp_iter */
                 0, /* tp_iternext */
                 spamobj_methods, /* tp_methods */
                 0, /* tp_members */
                 0, /* tp_getset */
                 0, /* tp_base */
                 0, /* tp_dict */
                 0, /* tp_descr_get */
                 0, /* tp_descr_set */
                 0, /* tp_dictoffset */
                 0, /* tp_init */
                 PyType_GenericAlloc, /* tp_alloc */
                 0, /* tp_new */
                 PyObject_GC_Del, /* tp_free */
             }},
    .gtd_size = 1,
    .gtd_new = PyType_GenericNew,
};

static int xxclassloader_exec(PyObject* m) {
  /* Fill in deferred data addresses.  This must be done before
     PyType_Ready() is called.  Note that PyType_Ready() automatically
     initializes the ob.ob_type field to &PyType_Type if it's NULL,
     so it's not necessary to fill in ob_type first. */
  if (PyType_Ready((PyTypeObject*)&spamobj_type) < 0) {
    return -1;
  }

  Py_INCREF(&spamobj_type);
  if (PyModule_AddObject(m, "spamobj", (PyObject*)&spamobj_type) < 0) {
    return -1;
  }

  return 0;
}

static struct PyModuleDef_Slot xxclassloader_slots[] = {
    {Py_mod_exec, xxclassloader_exec},
    {0, NULL},
};

static int no_op_visit(PyObject* obj, PyObject* Py_UNUSED(args)) {
  if (PyObject_IS_GC(obj) && PyObject_GC_IsTracked(obj)) {
    return 0;
  }

  /* visit objects not discoverable through GC  */
  if (Py_TYPE(obj)->tp_traverse == 0) {
    return 0;
  }

  /* tp_traverse can not be called for non-heap type object  */
  if (PyType_Check(obj) &&
      !PyType_HasFeature((PyTypeObject*)(obj), Py_TPFLAGS_HEAPTYPE)) {
    return 0;
  }

  Py_TYPE(obj)->tp_traverse(obj, (visitproc)no_op_visit, NULL);

  return 0;
}

int traverse_visitor(PyObject* obj, void* arg) {
  no_op_visit(obj, arg);
  Py_TYPE(obj)->tp_traverse(obj, (visitproc)no_op_visit, arg);
  return 1;
}

static PyObject* traverse_heap(
    PyObject* Py_UNUSED(self),
    PyObject* Py_UNUSED(args)) {
  PyUnstable_GC_VisitObjects(traverse_visitor, NULL /* arg */);
  Py_RETURN_NONE;
}

PyObject*
unsafe_change_type(PyObject* self, PyObject** args, Py_ssize_t nargs) {
  PyObject* obj;
  PyObject* new_type;
  if (!_PyArg_ParseStack(
          args, nargs, "OO:unsafe_change_type", &obj, &new_type)) {
    return NULL;
  }
  if (Py_TYPE(new_type) != &PyType_Type) {
    PyErr_SetString(PyExc_TypeError, "second argument must be a type");
    return NULL;
  }
  PyObject* old_type = (PyObject*)obj->ob_type;
  Py_INCREF(new_type);
  obj->ob_type = (PyTypeObject*)new_type;
  Py_DECREF(old_type);
  Py_RETURN_NONE;
}

static PyMethodDef xxclassloader_methods[] = {
    {"traverse_heap", traverse_heap, METH_NOARGS},
    {"unsafe_change_type",
     (PyCFunction)(void (*)(void))unsafe_change_type,
     METH_FASTCALL},
    {}};

static struct PyModuleDef xxclassloadermodule = {
    PyModuleDef_HEAD_INIT,
    "xxclassloader",
    xxclassloader__doc__,
    0,
    xxclassloader_methods,
    xxclassloader_slots,
    NULL,
    NULL,
    NULL};

int _Ci_CreateXXClassLoaderModule(void) {
  PyObject* mod =
      _Ci_CreateBuiltinModule(&xxclassloadermodule, "xxclassloader");
  if (mod == NULL) {
    return -1;
  }
  Py_DECREF(mod);
  return 0;
}

// These are used in native calling tests, ensure the compiler
// doesn't hide or remove these symbols
__attribute__((used)) __attribute__((visibility("default"))) int64_t
native_add(int64_t a, int64_t b) {
  return a + b;
}

__attribute__((used)) __attribute__((visibility("default"))) int64_t
native_sub(int64_t a, uint8_t b) {
  return a - b;
}
