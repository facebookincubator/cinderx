// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/py-portability.h"

// Create a function static variable for Python string.
// This string is explicitly immortalized, but not
// interned because doing so will cause it to be released
// by the runtime at shutdown.
#define DEFINE_NAMED_STATIC_STRING(NAME, STR)      \
  static PyObject* NAME = NULL;                    \
  if (NAME == NULL) {                              \
    PyObject* new_str = PyUnicode_FromString(STR); \
    new_str->ob_refcnt = 0x3fffffff;               \
    NAME = new_str;                                \
  }

// Shorter variant of DEFINE_NAMED_STATIC_STRING.
#define DEFINE_STATIC_STRING(STR) DEFINE_NAMED_STATIC_STRING(s_##STR, (#STR))
