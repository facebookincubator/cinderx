// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Common/py-portability.h"

// Create a function static variable for an interned Python string.
// This string is explicitly immortalized.
#define DEFINE_NAMED_STATIC_STRING(NAME, STR)            \
  static PyObject* NAME = NULL;                          \
  if (NAME == NULL) {                                    \
    PyObject* new_str = PyUnicode_InternFromString(STR); \
    IMMORTALIZE(new_str);                                \
    NAME = new_str;                                      \
  }

// Shorter variant of DEFINE_NAMED_STATIC_STRING.
#define DEFINE_STATIC_STRING(STR) DEFINE_NAMED_STATIC_STRING(s_##STR, (#STR))
