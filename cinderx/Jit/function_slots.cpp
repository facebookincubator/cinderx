// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/function_slots.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/context.h"

namespace cinderx::jit {

namespace {

const traverseproc original_func_traverse = PyFunction_Type.tp_traverse;
const inquiry original_func_clear = PyFunction_Type.tp_clear;

// A JIT-compiled function owns one reference to its CompiledFunction that is
// held logically rather than through any field of the function, so the base
// func_traverse() cannot see it.  Report it here or the collector will think
// the CompiledFunction is unreachable and free it out from under us.
//
// Deliberately takes no lock: this always runs either under the GIL or, on
// free-threaded builds, with the world stopped, and blocking on a lock held by
// a paused thread would deadlock the collector.  See Context::lookupCode.
int jitFuncTraverse(PyObject* self, visitproc visit, void* arg) {
  Context* ctx = getContext();
  if (ctx != nullptr) {
    BorrowedRef<PyFunctionObject> func{self};
    // If a deopted function gets collected we want the code to get
    // collected too but it's vectorcall points at the interpreted entry.
    BorrowedRef<CompiledFunction> compiled = ctx->deoptedCompile(func);
    if (compiled == nullptr) {
      compiled = ctx->lookupFunc(func);
      if (compiled != nullptr &&
          compiled->vectorcallEntry() != func->vectorcall) {
        compiled = nullptr;
      }
    }
    if (isCollectableCompile(compiled)) {
      Py_VISIT(reinterpret_cast<PyObject*>(compiled.get()));
    }
    // Also report any nested functions' compiles this function is holding on
    // their behalf, so that edge is visible to the collector and the cycle
    // through the CompiledFunction stays breakable.
    if (ctx->hasNestedCompiles()) {
      int res = ctx->traverseNestedCompiles(func, visit, arg);
      if (res != 0) {
        return res;
      }
    }
  }
  return original_func_traverse(self, visit, arg);
}

// Release the logical reference when the collector breaks a cycle.  Note this
// is only reached from the GC: func_dealloc() calls the static func_clear()
// directly rather than going through the type, so the ordinary destruction path
// drops the reference from the function watcher instead (jit::funcDestroyed).
int jitFuncClear(PyObject* self) {
  Context* ctx = getContext();
  if (ctx != nullptr) {
    // Must happen before the base implementation, which clears func_globals and
    // func_builtins and so makes the compilation key unrecoverable.
    ctx->releaseCompiledFuncRef(BorrowedRef<PyFunctionObject>{self});
    ctx->releaseNestedCompiles(BorrowedRef<PyFunctionObject>{self});
  }
  return original_func_clear(self);
}

} // namespace

void initJitFunctionSlots() {
  JIT_CHECK(
      original_func_traverse != nullptr && original_func_clear != nullptr,
      "PyFunction_Type is missing its GC slots");
  JIT_CHECK(
      PyFunction_Type.tp_traverse == original_func_traverse &&
          PyFunction_Type.tp_clear == original_func_clear,
      "PyFunction_Type GC slots already overridden");
  PyFunction_Type.tp_traverse = jitFuncTraverse;
  PyFunction_Type.tp_clear = jitFuncClear;
}

void shutdownJitFunctionSlots() {
  PyFunction_Type.tp_traverse = original_func_traverse;
  PyFunction_Type.tp_clear = original_func_clear;
}

} // namespace cinderx::jit
