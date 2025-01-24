// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if PY_VERSION_HEX < 0x030C0000

#include "Objects/dict-common.h" // @donotremove
#define DICT_VALUES(dict) dict->ma_values

#else

#define DICT_VALUES(dict) dict->ma_values->values
#include "internal/pycore_dict.h"

#endif

static inline PyObject* getBorrowedTypeDict(PyTypeObject* self) {
#if PY_VERSION_HEX >= 0x030C0000
  return _PyType_GetDict(self);
#else
  assert(self->tp_dict != NULL);
  return self->tp_dict;
#endif
}

#if PY_VERSION_HEX >= 0x030C0000
#define _PyDict_NotifyEvent(EVENT, MP, KEY, VAL) \
  _PyDict_NotifyEvent(_PyInterpreterState_GET(), (EVENT), (MP), (KEY), (VAL))
#endif

// Check if a dictionary is guaranteed to only contain unicode/string keys.
//
// Does not scan the dictionary, so if internally the dictionary is a
// "general-purpose" kind but happens to only contain strings this will still
// return false.
static inline bool hasOnlyUnicodeKeys(PyObject* dict) {
  assert(PyDict_Check(dict));

#if PY_VERSION_HEX >= 0x030C0000
  return DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys);
#else
  return _PyDict_HasOnlyUnicodeKeys(dict);
#endif
}

#ifdef __cplusplus
} // extern "C"
#endif
