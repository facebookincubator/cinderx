// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_function.h"
#endif

#if PY_VERSION_HEX < 0x030D0000 && defined(ENABLE_EVAL_HOOK)
#include "cinder/hooks.h"
#endif

extern "C" {

vectorcallfunc getInterpretedVectorcall(
    [[maybe_unused]] const PyFunctionObject* func) {
#ifdef ENABLE_INTERPRETER_LOOP
  const PyCodeObject* code = (const PyCodeObject*)(func->func_code);
  return (code->co_flags & CI_CO_STATICALLY_COMPILED)
      ? Ci_StaticFunction_Vectorcall
      : Ci_PyFunction_Vectorcall;
#else
  return Ci_PyFunction_Vectorcall;
#endif
}

int Ci_InitFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
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
#endif
#endif

  return 0;
}

void Ci_FiniFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = nullptr;
#elif defined(ENABLE_PEP523_HOOK)
  _PyInterpreterState_SetEvalFrameFunc(_PyInterpreterState_GET(), nullptr);
#endif
#endif
}

} // extern "C"
