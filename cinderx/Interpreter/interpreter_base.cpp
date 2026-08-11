// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_function.h"
#endif

#if PY_VERSION_HEX < 0x030D0000 && defined(ENABLE_EVAL_HOOK)
#include "cinder/hooks.h"
#endif

extern "C" {

int Ci_InitFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
  Ci_SetStaticFunctionVectorcall(Ci_StaticFunction_Vectorcall);
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = Ci_EvalFrame;
#elif defined(ENABLE_PEP523_HOOK)
  // Let borrowed.h know the eval frame pointer
  Ci_EvalFrameFunc = Ci_EvalFrame;

  auto interp = _PyInterpreterState_GET();
  auto current_eval_frame = _PyInterpreterState_GetEvalFrameFunc(interp);
  if (current_eval_frame == Ci_EvalFrame) {
    return 0;
  }
  if (current_eval_frame != _PyEval_EvalFrameDefault) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "CinderX tried to set a frame evaluator function but something else "
        "has done it first, this is not supported");
    return -1;
  }

  _PyInterpreterState_SetEvalFrameFunc(interp, Ci_EvalFrame);
#if PY_VERSION_HEX >= 0x030F0000
  _PyInterpreterState_SetEvalFrameAllowSpecialization(interp, 1);
#endif
#endif
#endif

  return 0;
}

void Ci_FiniFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
  Ci_SetStaticFunctionVectorcall(nullptr);
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = nullptr;
#elif defined(ENABLE_PEP523_HOOK)
  _PyInterpreterState_SetEvalFrameFunc(_PyInterpreterState_GET(), nullptr);
#endif
#endif
}

} // extern "C"
