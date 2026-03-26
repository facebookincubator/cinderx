// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Specifies the offset from a JITed function entry point where the re-entry
// point for calling with the correct bound args lives.
#if defined(__x86_64__)
// push rbp
// mov rbp, rsp
// jmp correct-bound-args
#define JITRT_CALL_REENTRY_OFFSET (-6)
#elif defined(__aarch64__)
// stp fp, lr, [sp, #-32]!
// mov fp, sp
// b correct-bound-args
#define JITRT_CALL_REENTRY_OFFSET (-12)
#else
#define JITRT_CALL_REENTRY_OFFSET (0)
#endif

// Fixes the JITed function entry point up to be the re-entry point after
// binding the args.
#define JITRT_GET_REENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_CALL_REENTRY_OFFSET))

// Specifies the offset from a JITed function entry point where the static
// entry point lives.
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define JITRT_STATIC_ENTRY_OFFSET (-11)
#elif defined(__aarch64__)
#define JITRT_STATIC_ENTRY_OFFSET (-16)
#else
// Without JIT support there's no entry offset.
#define JITRT_STATIC_ENTRY_OFFSET (0)
#endif

// Fixes the JITed function entry point up to be the static entry point after
// binding the args.
#define JITRT_GET_STATIC_ENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_STATIC_ENTRY_OFFSET))

// Fixes the JITed function entry point up to be the static entry point after
// binding the args.
#define JITRT_GET_NORMAL_ENTRY_FROM_STATIC(entry) \
  ((vectorcallfunc)(((char*)entry) - JITRT_STATIC_ENTRY_OFFSET))

/*
 * Check if a function has been compiled by Cinder's JIT and has a new
 * vectorcall entry point.
 *
 * Note: This returns false for the initial JIT entry points set by
 * scheduleJitCompile().
 */
bool isJitCompiled(const PyFunctionObject* func);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <unordered_set>
#include <utility>

namespace jit {

// Data members extracted from CompiledFunction to enable separate storage.
// CompiledFunction is a GC tracked object and cannot be constructed during
// multi-threaded compile because we can't hold the GIL which is required for
// allocating Python memory. This struct is used to pass around the data before
// we get the full object constructed.
struct CompiledFunctionData {
  std::span<const std::byte> code;
  vectorcallfunc vectorcall_entry{nullptr};
  int stack_size{0};
  int spill_stack_size{0};
  std::chrono::nanoseconds compile_time{};
  hir::Function::InlineFunctionStats inline_function_stats;
  hir::OpcodeCounts hir_opcode_counts{};
  // All the code patchers pointing to patch points in this function.
  std::vector<std::unique_ptr<CodePatcher>> code_patchers;
  std::unique_ptr<hir::Function> irfunc;
  CodeRuntime* runtime{nullptr};

  CompiledFunctionData() = default;
};

// The key used to store the CompiledFunction in a function's __dict__.
extern PyObject*
    kCompiledFunctionKey; // NOLINT(facebook-avoid-non-const-global-variables)
// The key used to store nested compiled functions in a function's __dict__.
extern PyObject*
    kNestedCompiledFunctionsKey; // NOLINT(facebook-avoid-non-const-global-variables)

class CompiledFunction;

class CompiledFunctionOwner {
 public:
  virtual ~CompiledFunctionOwner() = default;

  // Provides a notification to our owner when we are about to be destroyed
  virtual void forgetCompiledFunction(CompiledFunction& function) = 0;

  virtual void unwatch(TypeDeoptPatcher*) = 0;
};

// CompiledFunction is a Python GC object that contains a pointer to the native
// code compiled for a Python function. The memory behind the generated native
// code is in the CodeAllocator.
//
// CompiledFunction is reference counted via Python's GC. The Context holds
// strong references via Ref<CompiledFunction>. Functions also hold references
// via their __dict__.
class CompiledFunction {
 public:
  PyObject_HEAD
  // Factory function to create a new CompiledFunction.
  [[nodiscard]] static Ref<CompiledFunction> create(
      CompiledFunctionData&& data,
      bool immortal);

  ~CompiledFunction();

  // Get the buffer containing the compiled machine code.  The start of this
  // buffer is not guaranteed to be a valid entry point.
  std::span<const std::byte> codeBuffer() const {
    return data_.code;
  }

  vectorcallfunc vectorcallEntry() const {
    return data_.vectorcall_entry;
  }

  void* staticEntry() const;

  CodeRuntime* runtime() const {
    return data_.runtime;
  }

  PyObject* invoke(PyObject* func, PyObject** args, Py_ssize_t nargs) const {
    return data_.vectorcall_entry(func, args, nargs, nullptr);
  }

  void printHIR() const;
  void disassemble() const;

  size_t codeSize() const {
    return data_.code.size();
  }

  int stackSize() const {
    return data_.stack_size;
  }

  int spillStackSize() const {
    return data_.spill_stack_size;
  }

  std::chrono::nanoseconds compileTime() const;
  void setCompileTime(std::chrono::nanoseconds time);

  void setCodePatchers(
      std::vector<std::unique_ptr<CodePatcher>>&& code_patchers);

  void setHirFunc(std::unique_ptr<hir::Function>&& irfunc);

  const hir::Function::InlineFunctionStats& inlinedFunctionsStats() const {
    return data_.inline_function_stats;
  }

  const hir::OpcodeCounts& hirOpcodeCounts() const {
    return data_.hir_opcode_counts;
  }

  // Associate a function with this CompiledFunction. The function will be
  // tracked and deopted if the CompiledFunction is cleared.
  void addFunction(BorrowedRef<PyFunctionObject> func);

  // Remove a function from the set of associated functions.
  void removeFunction(BorrowedRef<PyFunctionObject> func);

  void setOwner(CompiledFunctionOwner* owner) {
    owner_ = owner;
  }

  std::unordered_set<BorrowedRef<PyFunctionObject>>& functions() {
    return functions_;
  }

  // Traverse all GC-reachable objects for the GC.
  int traverse(visitproc visit, void* arg);

  // Clear all references held by this CompiledFunction and deopt all
  // associated functions.
  void clear(bool context_finalizing = false);

 private:
  explicit CompiledFunction(CompiledFunctionData&& data)
      : data_(std::move(data)) {}

  friend Ref<CompiledFunction>;

  DISALLOW_COPY_AND_ASSIGN(CompiledFunction);

  CompiledFunctionOwner* owner_{};
  CompiledFunctionData data_;
  // Set of functions that are using this CompiledFunction.
  // These are borrowed references - the functions are responsible for removing
  // themselves via funcDestroyed() when they are deallocated.
  std::unordered_set<BorrowedRef<PyFunctionObject>> functions_;
};

// Initialize the CompiledFunction key and type. Should be called during
// module initialization.
int initCompiledFunctionType();

BorrowedRef<PyTypeObject> getCompiledFunctionType();

// Associate a function with a CompiledFunction and store a reference to the
// CompiledFunction in the function's __dict__.
bool associateFunctionWithCompiled(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled,
    bool is_nested);

} // namespace jit

#endif
