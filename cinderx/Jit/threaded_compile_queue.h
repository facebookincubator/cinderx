// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/threaded_compile.h"

#include <optional>
#include <vector>

namespace cinderx::jit {
// Single unit of compilation. We always compile with a Preloader which holds
// strong references to keep the compiled target alive. We may also optionally
// have a function which we keep alive across the compile as well. We won't have
// a function object if we're compiling a nested code object for which no
// function has been created yet.
struct CompilationUnit {
  const hir::Preloader* preloader;
  Ref<PyFunctionObject> func;

  CompilationUnit(const hir::Preloader* preloader, Ref<PyFunctionObject>&& func)
      : preloader(preloader), func(std::move(func)) {}
};

class ThreadedCompileQueue : public ThreadedCompileContext {
 public:
  using WorkList = std::vector<CompilationUnit>;

  ~ThreadedCompileQueue();
  ThreadedCompileQueue(const ThreadedCompileQueue&) = delete;
  ThreadedCompileQueue(ThreadedCompileQueue&&) = delete;
  ThreadedCompileQueue& operator=(const ThreadedCompileQueue&) = delete;
  ThreadedCompileQueue& operator=(ThreadedCompileQueue&&) = delete;

  // Used for batch multi-threaded compilation.
  explicit ThreadedCompileQueue(WorkList&& work_list);

  // Stop the current iteration of the multi-threaded compile, and return the
  // list of translation units that failed to compile.
  WorkList finalizeCompile();

  std::optional<CompilationUnit> nextUnit();

  // Mark a unit as having failed to compile and to be retried in the future.
  void retryUnit(CompilationUnit&& unit);

  // Mark a unit as being compiled and store it's reference for releasing later.
  void retireUnit(CompilationUnit&& unit);

 private:
  // List of translation units to iterate through and compile.
  WorkList work_list_;

  // List of translation units that have failed to compile.
  WorkList retry_list_;

  // References handed back by workers, released by endCompile().
  WorkList retired_list_;
};

} // namespace cinderx::jit
