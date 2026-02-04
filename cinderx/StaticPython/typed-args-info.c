// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/typed-args-info.h"

static void typedargsinfodealloc(_PyTypedArgsInfo* args_info) {
  PyObject_GC_UnTrack((PyObject*)args_info);
  for (Py_ssize_t i = 0; i < Py_SIZE(args_info); i++) {
    Py_XDECREF(args_info->tai_args[i].tai_type);
  }
  PyObject_GC_Del((PyObject*)args_info);
}

static int
typedargsinfotraverse(_PyTypedArgsInfo* args_info, visitproc visit, void* arg) {
  for (Py_ssize_t i = 0; i < Py_SIZE(args_info); i++) {
    Py_VISIT(args_info->tai_args[i].tai_type);
  }
  return 0;
}

static int typedargsinfoclear(_PyTypedArgsInfo* args_info) {
  for (Py_ssize_t i = 0; i < Py_SIZE(args_info); i++) {
    Py_CLEAR(args_info->tai_args[i].tai_type);
  }
  return 0;
}

PyTypeObject _PyTypedArgsInfo_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "typed_args_info",
    sizeof(_PyTypedArgsInfo),
    sizeof(_PyTypedArgsInfo),
    .tp_dealloc = (destructor)typedargsinfodealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_TUPLE_SUBCLASS, /* tp_flags */
    .tp_traverse = (traverseproc)typedargsinfotraverse,
    .tp_clear = (inquiry)typedargsinfoclear,
};
