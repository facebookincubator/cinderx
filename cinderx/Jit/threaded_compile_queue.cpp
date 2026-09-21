// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/threaded_compile_queue.h"

#include "cinderx/Jit/compilation_lock.h"

namespace cinderx::jit {

ThreadedCompileQueue::ThreadedCompileQueue(WorkList&& work_list) {
  work_list_ = std::move(work_list);
}

ThreadedCompileQueue::~ThreadedCompileQueue() {
  finalizeCompile();
}

ThreadedCompileQueue::WorkList ThreadedCompileQueue::finalizeCompile() {
  if (endCompile()) {
    // The GIL is held again, so it is safe to drop the references the workers
    // handed back.
    retired_list_.clear();
    return std::move(retry_list_);
  }
  return {};
}

std::optional<CompilationUnit> ThreadedCompileQueue::nextUnit() {
  std::optional<CompilationUnit> unit;
  JITCompilationLock lock;
  if (!work_list_.empty()) {
    unit = std::move(work_list_.back());
    work_list_.pop_back();
  }
  return unit;
}

void ThreadedCompileQueue::retryUnit(CompilationUnit&& unit) {
  JITCompilationLock lock;
  retry_list_.emplace_back(std::move(unit));
}

void ThreadedCompileQueue::retireUnit(CompilationUnit&& unit) {
  JITCompilationLock lock;
  retired_list_.emplace_back(std::move(unit));
}

} // namespace cinderx::jit
