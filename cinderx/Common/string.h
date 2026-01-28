// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/py-portability.h"

// Create a function static variable for Python string. This string is
// explicitly immortalized, but not interned because doing so will cause it to
// be released by the runtime at shutdown. We cannot use
// _Py_SetImmortalUntracked() as this has an assertion to force the use of
// _PyUnicode_InternImmortal() for strings.
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
#define DEFINE_NAMED_STATIC_STRING(NAME, STR)                  \
  static PyObject* NAME = NULL;                                \
  if (NAME == NULL) {                                          \
    PyObject* op = PyUnicode_FromString(STR);                  \
    op->ob_tid = _Py_UNOWNED_TID;                              \
    op->ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL;              \
    op->ob_ref_shared = 0;                                     \
    _Py_atomic_or_uint8(&op->ob_gc_bits, _PyGC_BITS_DEFERRED); \
    _PyASCIIObject_CAST(op)->state.statically_allocated = 1;   \
    NAME = op;                                                 \
  }
#else
#define DEFINE_NAMED_STATIC_STRING(NAME, STR)      \
  static PyObject* NAME = NULL;                    \
  if (NAME == NULL) {                              \
    PyObject* new_str = PyUnicode_FromString(STR); \
    new_str->ob_refcnt = 0x3fffffff;               \
    NAME = new_str;                                \
  }
#endif

// Shorter variant of DEFINE_NAMED_STATIC_STRING.
#define DEFINE_STATIC_STRING(STR) DEFINE_NAMED_STATIC_STRING(s_##STR, (#STR))
