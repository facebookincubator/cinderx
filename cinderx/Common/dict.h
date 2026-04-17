// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

// This needs to come before borrowed.h
#include "pycore_dict.h"

#ifdef __cplusplus
#include "cinderx/Common/ref.h"
#endif

#include "cinderx/UpstreamBorrow/borrowed.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICT_VALUES(dict) dict->ma_values->values
#include "internal/pycore_dict.h"

static inline PyObject* getBorrowedTypeDict(PyTypeObject* self) {
  return _PyType_GetDict(self);
}

#define _PyDict_NotifyEvent(EVENT, MP, KEY, VAL) \
  _PyDict_NotifyEvent(_PyInterpreterState_GET(), (EVENT), (MP), (KEY), (VAL))

// Check if a dictionary is guaranteed to only contain unicode/string keys.
//
// Does not scan the dictionary, so if internally the dictionary is a
// "general-purpose" kind but happens to only contain strings this will still
// return false.
static inline bool hasOnlyUnicodeKeys(PyObject* dict) {
  assert(PyDict_Check(dict));

  return DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys);
}

static inline Py_ssize_t getDictKeysIndex(
    PyDictKeysObject* keys,
    PyObject* name) {
#if PY_VERSION_HEX >= 0x030E0000
  return _PyDictKeys_StringLookupSplit(keys, name);
#endif
  for (Py_ssize_t i = 0; i < keys->dk_nentries; i++) {
    PyDictUnicodeEntry* ep = &DK_UNICODE_ENTRIES(keys)[i];
    if (PyUnicode_Compare(name, ep->me_key) == 0) {
      return i;
    }
  }
  return -1;
}

// We can't borrow this from CPython because it exists but is not
// exported, and therefore borrowing it duplicates the symbol.
static inline uint32_t dictGetKeysVersion(
    PyInterpreterState* interp,
    PyDictKeysObject* dictkeys) {
  if (dictkeys->dk_version != 0) {
    return dictkeys->dk_version;
  }
  if (interp->dict_state.next_keys_version == 0) {
    return 0;
  }
  uint32_t v = interp->dict_state.next_keys_version++;
  dictkeys->dk_version = v;
  return v;
}

#if PY_VERSION_HEX >= 0x030E0000
typedef uint32_t ci_dict_version_tag_t;
static inline ci_dict_version_tag_t Ci_DictVersionTag(PyDictObject* dict) {
  return _PyDict_GetKeysVersionForCurrentState(_PyInterpreterState_GET(), dict);
}
#else
typedef uint64_t ci_dict_version_tag_t;
static inline ci_dict_version_tag_t Ci_DictVersionTag(PyDictObject* dict) {
  return dict->ma_version_tag;
}
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

inline Ref<> getDictRef(PyObject* dict, PyObject* key) {
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* res;
  if (PyDict_GetItemRef(dict, key, &res) > 0) {
    return Ref<>::steal(res);
  }
#else
  PyObject* res = PyDict_GetItemWithError(dict, key);
  if (res != nullptr) {
    return Ref<>::create(res);
  }
#endif
  return nullptr;
}

#endif
