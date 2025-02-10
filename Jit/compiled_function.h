// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Specifies the offset from a JITed function entry point where the re-entry
// point for calling with the correct bound args lives.
#define JITRT_CALL_REENTRY_OFFSET (-6)

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
#include "cinderx/Jit/hir/hir.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>

namespace jit {

// CompiledFunction contains the native code that was compiled for a Python
// function.
//
// It does not manage the memory behind the generated native code, that is done
// by the CodeAllocator.
class CompiledFunction {
 public:
  CompiledFunction(
      std::span<const std::byte> code,
      vectorcallfunc vectorcall_entry,
      void* static_entry,
      int stack_size,
      int spill_stack_size,
      hir::Function::InlineFunctionStats inline_function_stats,
      const hir::OpcodeCounts& hir_opcode_counts)
      : code_(code),
        vectorcall_entry_(vectorcall_entry),
        static_entry_(static_entry),
        stack_size_(stack_size),
        spill_stack_size_(spill_stack_size),
        inline_function_stats_(std::move(inline_function_stats)),
        hir_opcode_counts_(hir_opcode_counts) {}

  virtual ~CompiledFunction() = default;

  // Get the buffer containing the compiled machine code.  The start of this
  // buffer is not guaranteed to be a valid entry point.
  std::span<const std::byte> codeBuffer() const {
    return code_;
  }

  vectorcallfunc vectorcallEntry() const {
    return vectorcall_entry_;
  }

  void* staticEntry() const {
    return static_entry_;
  }

  PyObject* invoke(PyObject* func, PyObject** args, Py_ssize_t nargs) const {
    return vectorcall_entry_(func, args, nargs, NULL);
  }

  virtual void printHIR() const;
  virtual void disassemble() const;

  size_t codeSize() const {
    return code_.size();
  }

  int stackSize() const {
    return stack_size_;
  }

  int spillStackSize() const {
    return spill_stack_size_;
  }

  std::chrono::nanoseconds compileTime() const;
  void setCompileTime(std::chrono::nanoseconds time);

  void setHirFunc(std::unique_ptr<hir::Function>&& irfunc);

  const hir::Function::InlineFunctionStats& inlinedFunctionsStats() const {
    return inline_function_stats_;
  }

  const hir::OpcodeCounts& hirOpcodeCounts() const {
    return hir_opcode_counts_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(CompiledFunction);

  const std::span<const std::byte> code_;
  vectorcallfunc const vectorcall_entry_;
  void* const static_entry_;
  const int stack_size_;
  const int spill_stack_size_;
  std::chrono::nanoseconds compile_time_;
  hir::Function::InlineFunctionStats inline_function_stats_;
  hir::OpcodeCounts hir_opcode_counts_;
  std::unique_ptr<hir::Function> irfunc_;
};

} // namespace jit

#endif
