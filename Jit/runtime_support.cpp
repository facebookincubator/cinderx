// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/runtime_support.h"

#include <Python.h>
#include "cinderx/Common/log.h"
#include "internal/pycore_pyerrors.h"

#include "cinderx/Upgrade/upgrade_assert.h"  // @donotremove

namespace jit {

#if PY_VERSION_HEX < 0x030C0000
PyObject g_iterDoneSentinel = {
    _PyObject_EXTRA_INIT kImmortalInitialCount,
    nullptr};
#endif

PyObject* invokeIterNext(PyObject* iterator) {
  PyObject* val = (*iterator->ob_type->tp_iternext)(iterator);
  if (val != nullptr) {
    return val;
  }
  if (PyErr_Occurred()) {
    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
      return nullptr;
    }
    PyErr_Clear();
  }
#if PY_VERSION_HEX < 0x030C0000
  Py_INCREF(&g_iterDoneSentinel);
  return &g_iterDoneSentinel;
#else
  UPGRADE_ASSERT(IMMORTALIZATION_DIFFERENT)
  return nullptr;
#endif
}

} // namespace jit
