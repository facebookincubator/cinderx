// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/py-portability.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_weakref.h"
#endif

void Ci_ClearWeakRefs(PyObject* self, PyObject* weakrefs) {
#if PY_VERSION_HEX >= 0x030E0000
  FT_CLEAR_WEAKREFS(self, weakrefs);
#else
  if (weakrefs != NULL) {
    PyObject_ClearWeakRefs(self);
  }
#endif
}
