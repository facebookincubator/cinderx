/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/vtable.h"

static void vtabledealloc(_PyType_VTable* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->vt_slotmap);
  Py_XDECREF(op->vt_thunks);
  Py_XDECREF(op->vt_original);
  Py_XDECREF(op->vt_specials);

  for (Py_ssize_t i = 0; i < op->vt_size; i++) {
    Py_XDECREF(op->vt_entries[i].vte_state);
  }
  _PyType_GenericTypeRef* gtr = op->vt_gtr;
  if (gtr != NULL) {
    Py_CLEAR(gtr->gtr_gtd);
    for (Py_ssize_t i = 0; i < gtr->gtr_typeparam_count; i++) {
      Py_CLEAR(gtr->gtr_typeparams[i]);
    }
    PyMem_Free(gtr);
  }
  PyObject_GC_Del((PyObject*)op);
}

static int vtabletraverse(_PyType_VTable* op, visitproc visit, void* arg) {
  for (Py_ssize_t i = 0; i < op->vt_size; i++) {
    Py_VISIT(op->vt_entries[i].vte_state);
  }
  Py_VISIT(op->vt_original);
  Py_VISIT(op->vt_thunks);
  Py_VISIT(op->vt_specials);
  _PyType_GenericTypeRef* gtr = op->vt_gtr;
  if (gtr != NULL) {
    Py_VISIT(gtr->gtr_gtd);
    for (Py_ssize_t i = 0; i < gtr->gtr_typeparam_count; i++) {
      Py_VISIT(gtr->gtr_typeparams[i]);
    }
  }
  return 0;
}

static int vtableclear(_PyType_VTable* op) {
  for (Py_ssize_t i = 0; i < op->vt_size; i++) {
    Py_CLEAR(op->vt_entries[i].vte_state);
  }
  Py_CLEAR(op->vt_original);
  Py_CLEAR(op->vt_thunks);
  Py_CLEAR(op->vt_specials);
  _PyType_GenericTypeRef* gtr = op->vt_gtr;
  if (gtr != NULL) {
    Py_CLEAR(gtr->gtr_gtd);
    for (Py_ssize_t i = 0; i < gtr->gtr_typeparam_count; i++) {
      Py_CLEAR(gtr->gtr_typeparams[i]);
    }
  }
  return 0;
}

PyTypeObject _PyType_VTableType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable",
    sizeof(_PyType_VTable) - sizeof(_PyType_VTableEntry),
    sizeof(_PyType_VTableEntry),
    .tp_dealloc = (destructor)vtabledealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_TUPLE_SUBCLASS, /* tp_flags */
    .tp_traverse = (traverseproc)vtabletraverse,
    .tp_clear = (inquiry)vtableclear,
};

PyObject* _PyClassLoader_InvokeMethod(
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyObject* state = vtable->vt_entries[slot].vte_state;
  return PyObject_Vectorcall(state, args, nargsf, NULL);
}

StaticMethodInfo _PyClassLoader_LoadStaticMethod(
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject* self) {
  loadmethodfunc func = vtable->vt_entries[slot].vte_load;
  return func(vtable->vt_entries[slot].vte_state, self);
}
