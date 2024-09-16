// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/entry.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Interpreter/interpreter.h"

#include "cinderx/Jit/pyjit.h"

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Shadowcode/shadowcode.h"
#include "pycore_interp.h"
#endif

extern "C" {

namespace {

uint64_t countCalls(PyCodeObject* code) {
#if PY_VERSION_HEX < 0x030C0000
  // The interpreter will only increment up to the shadowcode threshold
  // PYSHADOW_INIT_THRESHOLD. After that, it will stop incrementing. If someone
  // sets -X jit-auto above the PYSHADOW_INIT_THRESHOLD, we still have to keep
  // counting.
  unsigned ncalls = code->co_mutable->ncalls;
  if (ncalls > PYSHADOW_INIT_THRESHOLD) {
    ncalls++;
    code->co_mutable->ncalls = ncalls;
  }
  return ncalls;
#else
  auto extra = codeExtra(code);
  return extra != nullptr ? extra->calls : 0;
#endif
}

_PyJIT_Result tryCompile(BorrowedRef<PyFunctionObject> func) {
  _PyJIT_Result result =
      jit::isJitUsable() ? _PyJIT_CompileFunction(func) : PYJIT_NOT_INITIALIZED;
  // Reset the function back to the interpreter if there was any non-retryable
  // failure.
  if (result != PYJIT_RESULT_OK && result != PYJIT_RESULT_RETRY) {
    func->vectorcall = getInterpretedVectorcall(func);
  }
  return result;
}

// Python function entry point when AutoJIT is enabled.
PyObject* autoJITVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called AutoJIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);

  auto func = reinterpret_cast<PyFunctionObject*>(func_obj);
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);

  // Interpret function as usual until it passes the call count threshold.
  if (countCalls(code) <= jit::getConfig().auto_jit_threshold) {
    auto entry = getInterpretedVectorcall(func);
    return entry(func_obj, stack, nargsf, kwnames);
  }

  if (tryCompile(func) == PYJIT_RESULT_PYTHON_EXCEPTION) {
    return nullptr;
  }

  JIT_DCHECK(
      func->vectorcall != autoJITVectorcall,
      "Auto-JIT left function as auto-JIT'able on {}",
      jit::repr(func->func_qualname));
  return func->vectorcall(func_obj, stack, nargsf, kwnames);
}

// Python function entry point when the JIT is enabled, but not AutoJIT.
PyObject* jitVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called JIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);
  auto func = reinterpret_cast<PyFunctionObject*>(func_obj);
  if (tryCompile(func) == PYJIT_RESULT_PYTHON_EXCEPTION) {
    return nullptr;
  }
  JIT_DCHECK(
      func->vectorcall != jitVectorcall,
      "Lazy JIT left function as lazy-JIT'able on {}",
      jit::repr(func->func_qualname));
  return func->vectorcall(func_obj, stack, nargsf, kwnames);
}

} // namespace

void scheduleJitCompile(PyFunctionObject* func) {
  JIT_DCHECK(
      !_PyJIT_IsCompiled(func),
      "Function {} is already compiled",
      jit::repr(func->func_qualname));

  if (_PyJIT_IsAutoJITEnabled()) {
    func->vectorcall = autoJITVectorcall;
    return;
  }

  func->vectorcall = jitVectorcall;
  if (!_PyJIT_RegisterFunction(func)) {
    func->vectorcall = getInterpretedVectorcall(func);
  }
}

bool isJitEntryFunction(vectorcallfunc func) {
  return func == autoJITVectorcall || func == jitVectorcall;
}

vectorcallfunc getInterpretedVectorcall(PyFunctionObject* func) {
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  return (code->co_flags & CI_CO_STATICALLY_COMPILED)
      ? Ci_StaticFunction_Vectorcall
      : _PyFunction_Vectorcall;
}

} // extern "C"
