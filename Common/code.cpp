// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/code.h"

#include "cinderx/Common/log.h"

extern "C" {

_Py_CODEUNIT* codeUnit(PyCodeObject* code) {
  PyObject* bytes_obj = PyCode_GetCode(code);
  JIT_DCHECK(
      PyBytes_CheckExact(bytes_obj),
      "Code object must have its instructions stored as a byte string");
  return (_Py_CODEUNIT*)PyBytes_AS_STRING(PyCode_GetCode(code));
}

size_t countInstrs(PyCodeObject* code) {
  return PyBytes_GET_SIZE(PyCode_GetCode(code)) / sizeof(_Py_CODEUNIT);
}

} // extern "C"
