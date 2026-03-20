// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create a function static variable for Python string. This string is
// explicitly immortalized, but not interned because doing so will cause it to
// be released by the runtime at shutdown. We cannot use
// _Py_SetImmortalUntracked() as this has an assertion to force the use of
// _PyUnicode_InternImmortal() for strings.
#define DEFINE_NAMED_STATIC_STRING(NAME, STR) \
  static PyObject* NAME = NULL;               \
  if ((NAME) == NULL) {                       \
    NAME = Ci_InitStaticStringImpl((STR));    \
  }

// Shorter variant of DEFINE_NAMED_STATIC_STRING.
#define DEFINE_STATIC_STRING(STR) DEFINE_NAMED_STATIC_STRING(s_##STR, (#STR))

// Helper for DEFINE_NAMED_STATIC_STRING.  Do not call this directly.
PyObject* Ci_InitStaticStringImpl(const char* s);

#ifdef __cplusplus
} // extern "C"
#endif
