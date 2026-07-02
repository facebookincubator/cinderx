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
// stp fp, lr, [sp, #-16]!
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
#define JITRT_STATIC_ENTRY_OFFSET (-19)
#elif defined(__aarch64__)
#define JITRT_STATIC_ENTRY_OFFSET (-28)
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

#include <chrono>
#include <cstddef>
#include <span>
#include <unordered_set>

namespace cinderx::jit {

// Data members extracted from CompiledFunction to enable separate storage.
// CompiledFunction is a GC tracked object and cannot be constructed during
// multi-threaded compile because we can't hold the GIL which is required for
// allocating Python memory. This struct is used to pass around the data before
// we get the full object constructed.
struct CompiledFunctionData {
  // If the resulting CompiledFunction object is immortal, then this data object
  // will be reallocated inline to the CompiledFunction and we will not use it
  // as a PyObject.
  //
  // If the CompiledFunction is not immortal, then this data object will be
  // refcounted and have its lifetime managed by the Python runtime.  This is
  // set up at the same time the CompiledFunction is created.
  PyObject_HEAD
  // The generated machine code for the code object.  Memory is owned by the
  // CodeAllocator.
  std::span<const std::byte> code;
  // Entry point for calling this code object with the vectorcall API.
  vectorcallfunc vectorcall_entry{nullptr};
  // The total stack size used by the code object.
  int stack_size{0};
  // The amount of stack used for spilled values.
  int spill_stack_size{0};
  // Time taken to compile this code object.  Doesn't include preloading time.
  std::chrono::nanoseconds compile_time{};
  // Stats about the functions inlined into this code object.
  hir::Function::InlineFunctionStats inline_function_stats;
  // Counts of the HIR opcodes emitted while compiling this code object.
  hir::OpcodeCounts hir_opcode_counts{};
  // All the code patchers pointing to patch points in this function.
  std::vector<std::unique_ptr<CodePatcher>> code_patchers;
  // The HIR representation of this code object.  Optional, only used when debug
  // logging.
  std::unique_ptr<hir::Function> irfunc;
  // Runtime state (code object, globals, builtins) shared with the code.  Owned
  // by the JIT Context's CodeRuntime slab, not by this struct.
  CodeRuntime* runtime{nullptr};

  CompiledFunctionData() = default;
};

// The key used to store the CompiledFunction in a function's __dict__.
extern PyObject* kCompiledFunctionKey;
// The key used to store nested compiled functions in a function's __dict__.
extern PyObject* kNestedCompiledFunctionsKey;

class CompiledFunction;

// Interface for the owner of a CompiledFunction.  The owner holds the
// bookkeeping that maps Python functions and compilation keys to their
// CompiledFunction, plus a type-watcher registry, and is notified as a
// CompiledFunction is torn down.
//
// Currently implemented by jit::Context.
class CompiledFunctionOwner {
 public:
  virtual ~CompiledFunctionOwner() = default;

  // Notify the owner that a CompiledFunction is about to be destroyed.
  virtual void forgetCompiledFunction(CompiledFunction& function) = 0;

  // Unwatch a single TypeDeoptPatcher from a CompiledFunction.  The watched
  // Python type itself is left watched for any other patchers.
  virtual void unwatch(TypeDeoptPatcher* patcher) = 0;

  // Hand off a CompiledFunctionData for deferred destruction instead of freeing
  // it inline, since the machine code may still be executing on some thread's
  // stack.  The (code, builtins, globals) triple keys the deferred entry and
  // keeps the compilation data alive until cleanup determines no thread is
  // running the code.
  virtual void deferCompiledData(
      Ref<> code,
      Ref<> builtins,
      Ref<> globals,
      CompiledFunctionData* data) = 0;
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
  // factory function to create a new CompiledFunction.
  [[nodiscard]] static Ref<CompiledFunction> create(
      CompiledFunctionData&& data,
      bool immortal);

  ~CompiledFunction();

  // Get the buffer containing the compiled machine code.  The start of this
  // buffer is not guaranteed to be a valid entry point.
  std::span<const std::byte> codeBuffer() const;

  // Entry point for calling the function via the vectorcall API.
  vectorcallfunc vectorcallEntry() const;

  // Entry point for Static Python calls that skips argument boxing.  Returns
  // nullptr if the function was not statically compiled.
  void* staticEntry() const;

  // Runtime state (code object, globals, builtins) backing this function.
  CodeRuntime* runtime() const;

  // Call the compiled code directly through its vectorcall entry point.
  PyObject* invoke(PyObject* func, PyObject** args, Py_ssize_t nargs) const;

  void printHIR() const;
  void disassemble() const;

  // Size of the compiled machine code in bytes.
  size_t codeSize() const;

  // Total stack size used by the compiled code.
  int stackSize() const;

  // Stack size used for spilled values.
  int spillStackSize() const;

  std::chrono::nanoseconds compileTime() const;
  void setCompileTime(std::chrono::nanoseconds time);

  void setCodePatchers(
      std::vector<std::unique_ptr<CodePatcher>>&& code_patchers);

  void setHirFunc(std::unique_ptr<hir::Function>&& irfunc);

  const hir::Function::InlineFunctionStats& inlinedFunctionsStats() const;

  const hir::OpcodeCounts& hirOpcodeCounts() const;

  // Associate a function with this CompiledFunction. The function will be
  // tracked and deopted if the CompiledFunction is cleared.
  void addFunction(BorrowedRef<PyFunctionObject> func);

  // Remove a function from the set of associated functions.
  void removeFunction(BorrowedRef<PyFunctionObject> func);

  // Set the owner that is notified when this CompiledFunction is destroyed.
  void setOwner(CompiledFunctionOwner* owner);

  // The set of functions currently using this CompiledFunction.
  std::unordered_set<BorrowedRef<PyFunctionObject>>& functions();

  // Traverse all GC-reachable objects for the GC.
  int traverse(visitproc visit, void* arg);

  // Clear all references held by this CompiledFunction and deopt all
  // associated functions.
  void clear(bool context_finalizing = false);

  // Whether the data object is allocated contiguously with this object, which
  // is the case for immortal CompiledFunction objects.
  bool isContiguous() const;

  CompiledFunctionData* data() const;

  // Transfer ownership of the CompiledFunctionData out of this object.
  // After this call, the CF no longer owns or references the data.
  CompiledFunctionData* stealData();

 private:
  explicit CompiledFunction(CompiledFunctionData* data, bool contiguous);

  friend Ref<CompiledFunction>;

  DISALLOW_COPY_AND_ASSIGN(CompiledFunction);

  CompiledFunctionOwner* owner_{};
  CompiledFunctionData* data_{nullptr};
  bool contiguous_data_{false};
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

} // namespace cinderx::jit

#endif
