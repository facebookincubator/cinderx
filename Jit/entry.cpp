// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/entry.h"

#include "cinderx/Jit/pyjit.h"

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Shadowcode/shadowcode.h"
#include "pycore_interp.h"
#endif

namespace jit {
namespace {

void initFunctionObjectForStaticOrNonJIT(PyFunctionObject* func) {
  // Check that func hasn't already been initialized.
  JIT_DCHECK(
      func->vectorcall ==
          reinterpret_cast<vectorcallfunc>(
              Ci_JIT_lazyJITInitFuncObjectVectorcall),
      "Double initializing function {}",
      repr(func->func_qualname));
#if PY_VERSION_HEX < 0x030C0000
  if (((PyCodeObject*)func->func_code)->co_flags & CO_STATICALLY_COMPILED) {
    func->vectorcall =
        reinterpret_cast<vectorcallfunc>(Ci_StaticFunction_Vectorcall);
  } else {
    func->vectorcall = reinterpret_cast<vectorcallfunc>(_PyFunction_Vectorcall);
  }
#else
  UPGRADE_ASSERT(NEED_STATIC_FLAGS);
#endif
}

unsigned int count_calls(PyCodeObject* code) {
#if PY_VERSION_HEX < 0x030C0000
  // The interpreter will only increment up to the shadowcode threshold
  // PYSHADOW_INIT_THRESHOLD. After that, it will stop incrementing. If someone
  // sets -X jit-auto above the PYSHADOW_INIT_THRESHOLD, we still have to keep
  // counting.
  unsigned int ncalls = code->co_mutable->ncalls;
  if (ncalls > PYSHADOW_INIT_THRESHOLD) {
    ncalls++;
    code->co_mutable->ncalls = ncalls;
  }
  return ncalls;
#else
  UPGRADE_ASSERT(CHANGED_PYCODEOBJECT);
  return 0;
#endif
}

PyObject* autoJITFuncObjectVectorcall(
    PyFunctionObject* func,
    PyObject** stack,
    Py_ssize_t nargsf,
    PyObject* kwnames) {
  PyCodeObject* code = (PyCodeObject*)func->func_code;

  unsigned ncalls = count_calls(code);
  unsigned hot_threshold = _PyJIT_AutoJITThreshold();
  unsigned jit_threshold = hot_threshold + _PyJIT_AutoJITProfileThreshold();

  // If the function is found to be hot then register it to be profiled, and
  // enable interpreter profiling if it's not already enabled.
  if (ncalls == hot_threshold && hot_threshold != jit_threshold) {
#if PY_VERSION_HEX < 0x030C0000
    _PyJIT_MarkProfilingCandidate(code);
    PyThreadState* tstate = _PyThreadState_GET();
    if (!tstate->profile_interp) {
      tstate->profile_interp = 1;
      tstate->cframe->use_tracing = _Py_ThreadStateHasTracing(tstate);
    }
#else
    UPGRADE_ASSERT(PROFILING_CHANGED)
#endif
  }

  if (ncalls <= jit_threshold) {
    return _PyFunction_Vectorcall((PyObject*)func, stack, nargsf, kwnames);
  }

  // Function is about to be compiled, can stop profiling it now.  Disable
  // interpreter profiling if this is the last profiling candidate and we're
  // not profiling all bytecodes globally.
  if (hot_threshold != jit_threshold) {
#if PY_VERSION_HEX < 0x030C0000
    _PyJIT_UnmarkProfilingCandidate(code);
    PyThreadState* tstate = _PyThreadState_GET();
    if (tstate->profile_interp &&
        tstate->interp->ceval.profile_instr_period == 0 &&
        _PyJIT_NumProfilingCandidates() == 0) {
      tstate->profile_interp = 0;
      tstate->cframe->use_tracing = _Py_ThreadStateHasTracing(tstate);
    }
#else
    UPGRADE_ASSERT(PROFILING_CHANGED)
#endif
  }

  _PyJIT_Result result = _PyJIT_CompileFunction(func);
  if (result == PYJIT_RESULT_PYTHON_EXCEPTION) {
    return nullptr;
  } else if (result != PYJIT_RESULT_OK) {
    func->vectorcall = reinterpret_cast<vectorcallfunc>(
        Ci_JIT_lazyJITInitFuncObjectVectorcall);
    initFunctionObjectForStaticOrNonJIT(func);
  }
  JIT_DCHECK(
      func->vectorcall !=
          reinterpret_cast<vectorcallfunc>(autoJITFuncObjectVectorcall),
      "Auto-JIT left function as auto-JIT'able on {}",
      repr(func->func_qualname));
  return func->vectorcall((PyObject*)func, stack, nargsf, kwnames);
}

} // namespace

void initFunctionObjectForJIT(PyFunctionObject* func) {
  JIT_DCHECK(
      !_PyJIT_IsCompiled(func),
      "Function {} is already compiled",
      repr(func->func_qualname));
  if (_PyJIT_IsAutoJITEnabled()) {
    func->vectorcall =
        reinterpret_cast<vectorcallfunc>(autoJITFuncObjectVectorcall);
    return;
  }
  func->vectorcall =
      reinterpret_cast<vectorcallfunc>(Ci_JIT_lazyJITInitFuncObjectVectorcall);
  if (!_PyJIT_RegisterFunction(func)) {
    initFunctionObjectForStaticOrNonJIT(func);
  }
}

} // namespace jit

PyObject* Ci_JIT_lazyJITInitFuncObjectVectorcall(
    PyFunctionObject* func,
    PyObject** stack,
    Py_ssize_t nargsf,
    PyObject* kwnames) {
  if (!_PyJIT_IsEnabled()) {
    jit::initFunctionObjectForStaticOrNonJIT(func);
  } else {
    _PyJIT_Result result = _PyJIT_CompileFunction(func);
    if (result == PYJIT_RESULT_PYTHON_EXCEPTION) {
      return nullptr;
    } else if (result != PYJIT_RESULT_OK) {
      jit::initFunctionObjectForStaticOrNonJIT(func);
    }
  }
  JIT_DCHECK(
      func->vectorcall !=
          reinterpret_cast<vectorcallfunc>(
              Ci_JIT_lazyJITInitFuncObjectVectorcall),
      "Lazy JIT left function as lazy-JIT'able on {}",
      jit::repr(func->func_qualname));
  return func->vectorcall((PyObject*)func, stack, nargsf, kwnames);
}
