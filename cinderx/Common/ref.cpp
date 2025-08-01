// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#endif

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_pystate.h"
#endif

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
