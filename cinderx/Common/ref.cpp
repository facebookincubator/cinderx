// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#endif

#include "internal/pycore_pystate.h"

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
  interp->object_state.reftotal++;
#endif
}

void decref_total(PyInterpreterState* interp) {
#ifdef Py_REF_DEBUG
  interp->object_state.reftotal--;
#endif
}

#endif
