// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

// Get the callable out of a classmethod object.
static inline PyObject* Ci_PyClassMethod_GetFunc(PyObject* classmethod) {
  PyMemberDef* member = &Py_TYPE(classmethod)->tp_members[0];
  assert(strcmp(member->name, "__func__") == 0);
  Py_ssize_t offset = member->offset;
  return *((PyObject**)((char*)classmethod + offset));
}

// Get the callable out of a staticmethod object.
static inline PyObject* Ci_PyStaticMethod_GetFunc(PyObject* staticmethod) {
  // classmethod and staticmethod have the same underlying structure.
  return Ci_PyClassMethod_GetFunc(staticmethod);
}

#ifdef __cplusplus
} // extern "C"
#endif
