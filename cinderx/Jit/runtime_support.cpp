// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/runtime_support.h"

#include "cinderx/Immortalize/immortalize.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "refcount.h"
#endif

namespace jit {

PyObject g_iterDoneSentinel = {
    _PyObject_EXTRA_INIT
#if PY_VERSION_HEX >= 0x030E0000
    {.ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT},
#elif PY_VERSION_HEX >= 0x030C0000
    {.ob_refcnt = _Py_IMMORTAL_REFCNT},
#else
        _Py_IMMORTAL_REFCNT,
#endif
    nullptr};

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
  Py_INCREF(&g_iterDoneSentinel);
  return &g_iterDoneSentinel;
}

} // namespace jit
