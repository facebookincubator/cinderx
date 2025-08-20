// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/log.h"

#include "cinderx/Jit/threaded_compile.h"

namespace jit {

void printPythonException() {
#if PY_VERSION_HEX < 0x030C0000
  PyThreadState* tstate = PyThreadState_Get();
  if (tstate != nullptr && tstate->curexc_type != nullptr) {
    PyErr_Display(
        tstate->curexc_type, tstate->curexc_value, tstate->curexc_traceback);
  }
#else
  if (PyErr_Occurred()) {
    PyErr_DisplayException(PyErr_GetRaisedException());
  }
#endif
}

std::string repr(BorrowedRef<> obj) {
  jit::ThreadedCompileSerialize guard;

  PyObject *t, *v, *tb;

  PyErr_Fetch(&t, &v, &tb);
  auto p_str =
      Ref<PyObject>::steal(PyObject_Repr(const_cast<PyObject*>(obj.get())));
  PyErr_Restore(t, v, tb);
  if (p_str == nullptr) {
    return fmt::format(
        "<failed to repr Python object of type %s>", Py_TYPE(obj)->tp_name);
  }
  Py_ssize_t len;
  const char* str = PyUnicode_AsUTF8AndSize(p_str, &len);
  if (str == nullptr) {
    return "<failed to get UTF8 from Python string>";
  }
  return {str, static_cast<std::string::size_type>(len)};
}

} // namespace jit
