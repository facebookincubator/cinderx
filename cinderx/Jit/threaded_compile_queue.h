// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/threaded_compile.h"

#include <vector>

namespace cinderx::jit {
class ThreadedCompileQueue : public ThreadedCompileContext {
 public:
  using WorkList = std::vector<Ref<>>;

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

  Ref<> nextUnit();

  // Mark a unit as having failed to compile and to be retried in the future.
  void retryUnit(Ref<>&& unit);

  // Mark a unit as being compiled and store it's reference for releasing later.
  void retireUnit(Ref<>&& unit);

 private:
  // List of translation units to iterate through and compile.
  WorkList work_list_;

  // List of translation units that have failed to compile.
  WorkList retry_list_;

  // References handed back by workers, released by endCompile().
  WorkList retired_list_;
};

} // namespace cinderx::jit
