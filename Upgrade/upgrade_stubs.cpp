// Copyright (c) Meta Platforms, Inc. and affiliates.
#define __UPGRADE_STUBS_CPP

#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#define STUB(ret, func, task, args...)                \
  ret func(args) {                                    \
    UPGRADE_ASSERT(Hit stubbed function : func task); \
  }

extern "C" {
STUB(
    PyObject*,
    builtin_next,
    T196761974,
    PyObject*,
    PyObject* const*,
    Py_ssize_t)
// We added this and it's hard to get out of the runtime as it checks equality
// on a static function.
STUB(
    Py_ssize_t,
    _PyDictKeys_GetSplitIndex,
    T196879402,
    PyDictKeysObject*,
    PyObject*)

STUB(
    void,
    _PyType_ClearNoShadowingInstances,
    T197103405,
    struct _typeobject*,
    PyObject*)

} // extern "C"
