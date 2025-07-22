/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a new object key wrapper. An object key is just a simple
// wrapper which takes a borrowed reference and returns a wrapper
// around it which supports rich comparison to be equal to the
// original object. It's the callers responsibility to manage the
// lifetime of the original object to make sure it doesn't get
// recycled as a new object.
//
// The primary use case is for storing an object key inside of a
// dictionary. You can then use the original object to perform
// the lookup to get the original value, without keeping the object
// alive. This works even on objects which don't support weak refs.
PyObject* _Ci_ObjectKey_New(PyObject* obj);

typedef struct {
  PyObject_HEAD
  void* obj;
} _Ci_ObjectKey;

extern PyTypeObject _Ci_ObjectKeyType;

#ifdef __cplusplus
}
#endif
