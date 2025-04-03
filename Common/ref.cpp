#include "cinderx/Common/ref.h"

#include "internal/pycore_pystate.h"

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
