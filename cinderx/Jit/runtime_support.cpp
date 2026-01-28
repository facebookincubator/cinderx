// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/runtime_support.h"

// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Immortalize/immortalize.h"

#ifdef Py_GIL_DISABLED
#include "internal/pycore_gc.h"
#endif

#if PY_VERSION_HEX >= 0x030E0000
#include "refcount.h"
#endif

namespace jit {

PyObject g_iterDoneSentinel = {
    _PyObject_EXTRA_INIT
#if PY_VERSION_HEX >= 0x030E0000
// clang-format off
#ifdef Py_GIL_DISABLED
    .ob_tid = _Py_UNOWNED_TID,
    .ob_flags = _Py_STATICALLY_ALLOCATED_FLAG,
    .ob_mutex = {0},
    .ob_gc_bits = _PyGC_BITS_DEFERRED,
    .ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL,
    .ob_ref_shared = 0,
#else
    {.ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT,
     .ob_flags = _Py_STATIC_FLAG_BITS},
#endif
// clang-format on

#elif PY_VERSION_HEX >= 0x030C0000
    {.ob_refcnt = _Py_IMMORTAL_REFCNT},
#else
        _Py_IMMORTAL_REFCNT,
#endif
    nullptr};

PyObject* invokeIterNext(PyObject* iterator) {
  iternextfunc internext_f = Py_TYPE(iterator)->tp_iternext;
  // This check was introduced in 3.14+ but looks like it would be legit in all
  // versions. I'm surprised it wasn't backported.
  if (internext_f == nullptr) {
    PyErr_Format(
        PyExc_TypeError,
        "'%.100s' object is not an iterator",
        Py_TYPE(iterator)->tp_name);
    return nullptr;
  }
  PyObject* val = internext_f(iterator);
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
