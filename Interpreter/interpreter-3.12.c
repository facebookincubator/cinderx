// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Interpreter/opcode.h"

#define CINDERX_INTERPRETER
#include "Includes/ceval.c"

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState* tstate, _PyInterpreterFrame* frame, int throwflag) {
  return _PyEval_EvalFrameDefault(tstate, frame, throwflag);
}
