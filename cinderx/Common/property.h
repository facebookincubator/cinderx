// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Matches the layout of CPython's property object as that's not exposed
typedef struct {
  PyObject_HEAD
  PyObject* prop_get;
  PyObject* prop_set;
  PyObject* prop_del;
  PyObject* prop_doc;
  int getter_doc;
} Ci_propertyobject;

#ifdef __cplusplus
}
#endif
