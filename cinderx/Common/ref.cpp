// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#endif

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_pystate.h"
#endif

#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
#include "internal/pycore_tstate.h"

#include <atomic>
#endif

#ifdef Py_GIL_DISABLED

void incref_total(PyThreadState* tstate) {
#ifdef Py_REF_DEBUG
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  std::atomic_ref<Py_ssize_t>(tstate_impl->reftotal)
      .fetch_add(1, std::memory_order_relaxed);
#endif
}

void decref_total(PyThreadState* tstate) {
#ifdef Py_REF_DEBUG
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  std::atomic_ref<Py_ssize_t>(tstate_impl->reftotal)
      .fetch_sub(1, std::memory_order_relaxed);
#endif
}

#else

void incref_total(PyInterpreterState* interp) {
#ifdef Py_REF_DEBUG
#if PY_VERSION_HEX >= 0x030C0000
  interp->object_state.reftotal++;
#else
  _Py_RefTotal++;
#endif
#endif
}

void decref_total(PyInterpreterState* interp) {
#ifdef Py_REF_DEBUG
#if PY_VERSION_HEX >= 0x030C0000
  interp->object_state.reftotal--;
#else
  _Py_RefTotal--;
#endif
#endif
}

#endif
