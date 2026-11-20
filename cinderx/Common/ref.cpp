// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"

#include "cinderx/Common/runtime_offsets.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#endif

#include "internal/pycore_pystate.h"

#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
#include "internal/pycore_tstate.h"
#endif

namespace cinderx {

void incref_total([[maybe_unused]] PyThreadState* tstate) {
#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  _Py_atomic_add_ssize(&CiTstatePost(tstate_impl)->reftotal, 1);
#endif
}

void decref_total([[maybe_unused]] PyThreadState* tstate) {
#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  _Py_atomic_add_ssize(&CiTstatePost(tstate_impl)->reftotal, -1);
#endif
}

void incref_total([[maybe_unused]] PyInterpreterState* interp) {
#if defined(Py_REF_DEBUG) && !defined(Py_GIL_DISABLED)
  CiInterpPost(interp)->object_state.reftotal++;
#endif
}

void decref_total([[maybe_unused]] PyInterpreterState* interp) {
#if defined(Py_REF_DEBUG) && !defined(Py_GIL_DISABLED)
  CiInterpPost(interp)->object_state.reftotal--;
#endif
}

} // namespace cinderx
