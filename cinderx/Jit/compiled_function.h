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
#define JITRT_STATIC_ENTRY_OFFSET (-11)
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

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>

namespace jit {

// Data members extracted from CompiledFunction to enable separate storage.
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

// CompiledFunction contains a pointer to the native code that was compiled for
// a Python function.  The memory behind the generated native code is in the
// CodeAllocator.
class CompiledFunction {
 public:
  CompiledFunction(
      std::span<const std::byte> code,
      vectorcallfunc vectorcall_entry,
      int stack_size,
      int spill_stack_size,
      hir::Function::InlineFunctionStats inline_function_stats,
      const hir::OpcodeCounts& hir_opcode_counts,
      CodeRuntime* runtime) {
    data_.code = code;
    data_.vectorcall_entry = vectorcall_entry;
    data_.stack_size = stack_size;
    data_.spill_stack_size = spill_stack_size;
    data_.inline_function_stats = std::move(inline_function_stats);
    data_.hir_opcode_counts = hir_opcode_counts;
    data_.runtime = runtime;
  }

  explicit CompiledFunction(CompiledFunctionData&& data)
      : data_(std::move(data)) {}

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

 private:
  friend Ref<CompiledFunction>;

  DISALLOW_COPY_AND_ASSIGN(CompiledFunction);

  CompiledFunctionData data_;
};

} // namespace jit

#endif
