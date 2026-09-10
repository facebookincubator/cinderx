// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/context_iface.h"

#include <atomic>

namespace cinderx::jit {

class NestedCompileData {
 public:
  NestedCompileData(
      BorrowedRef<> module_name,
      BorrowedRef<PyCodeObject> code,
      JitEligibility eligibility)
      : module_name_{Ref<>::create(module_name)},
        code_{code},
        eligibility_{eligibility} {}

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  // Module name the code object was registered under, kept so that the cached
  // eligibility can be recomputed if the JIT list changes.
  BorrowedRef<> moduleName() const {
    return module_name_;
  }

  JitEligibility eligibility() const {
    return eligibility_.load(std::memory_order_acquire);
  }

  void setEligibility(JitEligibility eligibility) {
    eligibility_.store(eligibility, std::memory_order_release);
  }

  bool mayOwnCodeOuterFuncEntry() const {
    return may_own_code_outer_func_entry_.load(std::memory_order_acquire);
  }

  void markOwnsCodeOuterFuncEntry() {
    may_own_code_outer_func_entry_.store(true, std::memory_order_release);
  }

  ~NestedCompileData() {
    setCompiledFunction(nullptr);
  }

  BorrowedRef<CompiledFunction> compiledFunction() const {
    return compiled_function_.load(std::memory_order_acquire);
  }

  // The function the nested code was found in, which reports the compile below
  // to the garbage collector on this entry's behalf.  Borrowed, and null when
  // no compile is held.  See Context::addNestedCompile for why the reference
  // has to be reported by a Python object rather than kept as a C++ root.
  BorrowedRef<PyFunctionObject> outerFunc() const {
    return outer_func_;
  }

  void setOuterFunc(BorrowedRef<PyFunctionObject> outer) {
    outer_func_ = outer;
  }

  // This is an OWNING reference.  It is what keeps a nested function's compiled
  // code alive across the window where no function object is using it, so that
  // the next instance of the nested function reuses the same compile instead of
  // recompiling.  The reference lasts until the code object dies and the JIT
  // drops this entry (see Context::eraseNestedCompileData).
  void setCompiledFunction(BorrowedRef<CompiledFunction> compiled) {
    Py_XINCREF(compiled.get());
    CompiledFunction* old =
        compiled_function_.exchange(compiled.get(), std::memory_order_acq_rel);
    Py_XDECREF(old);
  }

  void clearCompiledFunction(BorrowedRef<CompiledFunction> compiled) {
    CompiledFunction* expected = compiled.get();
    if (compiled_function_.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel)) {
      Py_XDECREF(compiled.get());
    }
  }

  NestedCompileData(const NestedCompileData&) = delete;
  NestedCompileData& operator=(const NestedCompileData&) = delete;
  NestedCompileData(NestedCompileData&&) = delete;
  NestedCompileData& operator=(NestedCompileData&&) = delete;

 private:
  Ref<> module_name_;
  BorrowedRef<PyCodeObject> code_;
  std::atomic<JitEligibility> eligibility_;
  // Monotonic because false permits skipping ownership cleanup.
  std::atomic<bool> may_own_code_outer_func_entry_{false};
  std::atomic<CompiledFunction*> compiled_function_{nullptr};
  BorrowedRef<PyFunctionObject> outer_func_{nullptr};
};

// Attach data to its own code object, so that every path that creates a
// function can find it, not just the ones running from JIT-compiled code.  The
// JIT context stays the owner; this is only a back-pointer.  Does nothing if
// the code object can't hold a CodeExtra.
void publishNestedCompileData(NestedCompileData* data);

// Detach data from its code object, but only if it is still the attached data.
void unpublishNestedCompileData(NestedCompileData* data);

// Find the data attached to a code object, or nullptr if it has none.
NestedCompileData* nestedCompileData(BorrowedRef<PyCodeObject> code);

// Whether a code object's entry can speak for `func`.
//
// The entry is keyed by code object, and the eligibility it caches belongs to
// the name the code object carries.  A function normally borrows that name
// verbatim from its code, but __qualname__ and __module__ are writable and the
// JIT list matches on the function's copy.  Once they diverge the entry is
// answering for a different name, so it must not decide whether `func` gets
// compiled; `func` goes through the full per-function lookup instead.
//
// This says nothing about the compile the entry holds.  That is keyed by code
// object, globals and builtins, none of which a rename touches, so it is cached
// and reused across renamed instances -- see Context::codeCompiled.
inline bool nestedCompileDataMatches(
    BorrowedRef<PyFunctionObject> func,
    const NestedCompileData& data) {
  // We just compare pointers here because typically code objects aren't
  // ever changing.
  return func == nullptr ||
      (func->func_qualname == data.code()->co_qualname &&
       func->func_module == data.moduleName());
}

}; // namespace cinderx::jit
