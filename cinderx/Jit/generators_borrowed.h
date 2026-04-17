// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  PyObject_HEAD
  PyObject* wrapped;
  PyObject* default_value;
} anextawaitableobject;

void Ci_anextawaitable_dealloc(anextawaitableobject* obj);
int Ci_anextawaitable_traverse(
    anextawaitableobject* obj,
    visitproc visit,
    void* arg);
PyObject* Ci_anextawaitable_iternext(anextawaitableobject* obj);
PyObject* Ci_anextawaitable_send(anextawaitableobject* obj, PyObject* arg);
PyObject* Ci_anextawaitable_throw(anextawaitableobject* obj, PyObject* arg);

PyObject* Ci_anextawaitable_close(anextawaitableobject* obj, PyObject* arg);

#ifdef __cplusplus
} // extern "C"
#endif
