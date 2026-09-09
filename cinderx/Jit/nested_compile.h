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

  BorrowedRef<CompiledFunction> compiledFunction() const {
    return compiled_function_.load(std::memory_order_acquire);
  }

  void setCompiledFunction(BorrowedRef<CompiledFunction> compiled) {
    compiled_function_.store(compiled.get(), std::memory_order_release);
  }

  void clearCompiledFunction(BorrowedRef<CompiledFunction> compiled) {
    CompiledFunction* expected = compiled.get();
    compiled_function_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
  }

 private:
  Ref<> module_name_;
  BorrowedRef<PyCodeObject> code_;
  std::atomic<JitEligibility> eligibility_;
  // Monotonic because false permits skipping ownership cleanup.
  std::atomic<bool> may_own_code_outer_func_entry_{false};
  std::atomic<CompiledFunction*> compiled_function_{nullptr};
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

}; // namespace cinderx::jit
