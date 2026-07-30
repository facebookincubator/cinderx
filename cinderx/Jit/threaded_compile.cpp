// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/threaded_compile.h"

#include "cinderx/Common/py-portability.h"

namespace cinderx::jit {

thread_local ThreadedCompileContext* ThreadedCompileContext::current_ = nullptr;
thread_local PyThreadState* ThreadedCompileContext::threaded_compile_tstate_ =
    nullptr;
thread_local int ThreadedCompileContext::gil_lock_depth_ = 0;

ThreadedCompileContext::ThreadedCompileContext(WorkList&& work_list) {
  JIT_DCHECK(current_ == nullptr, "should have no current context");
  JIT_DCHECK(
      threaded_compile_tstate_ == nullptr, "should have no saved thread state");
  work_list_ = std::move(work_list);
  interpreter_ = PyInterpreterState_Get();
  current_ = this;
  threaded_compile_tstate_ = PyThreadState_Get();
}

ThreadedCompileContext::~ThreadedCompileContext() {
  if (!ended_) {
    endCompile();
  }
}

ThreadedCompileContext::WorkList ThreadedCompileContext::endCompile() {
  if (ended_) {
    return {};
  }

  if (main_ != nullptr) {
    // Re-acquire GIL for finalization.
    PyEval_RestoreThread(main_);
    main_ = nullptr;
  }
  ended_ = true;
  threaded_compile_tstate_ = nullptr;
  current_ = nullptr;
  return std::move(retry_list_);
}

void ThreadedCompileContext::releaseGil() {
  JIT_DCHECK(main_ == nullptr, "main already saved");
  main_ = PyEval_SaveThread();
}

BorrowedRef<> ThreadedCompileContext::nextUnit() {
  BorrowedRef<> unit;
  JITCompilationLock lock;
  if (!work_list_.empty()) {
    unit = std::move(work_list_.back());
    work_list_.pop_back();
  }
  return unit;
}

void ThreadedCompileContext::retryUnit(BorrowedRef<> unit) {
  JITCompilationLock lock;
  retry_list_.emplace_back(std::move(unit));
}

bool ThreadedCompileContext::compileRunning() {
  return threaded_compile_tstate_ != nullptr;
}

bool ThreadedCompileContext::canAccessSharedData() {
  return !compileRunning() || gil_lock_depth_ > 0 ||
      PyThreadState_GetUnchecked() != nullptr;
}

PyInterpreterState* ThreadedCompileContext::interpreter() {
  if (compileRunning()) {
    ThreadedCompileContext* cur = current_;
    JIT_DCHECK(
        cur != nullptr && cur->interpreter_ != nullptr,
        "should be set while compiling");
    return cur->interpreter_;
  }
  return PyInterpreterState_Get();
}

PyThreadState* ThreadedCompileContext::tstate() {
  if (compileRunning()) {
    return threaded_compile_tstate_;
  }
  return PyThreadState_Get();
}

void ThreadedCompileContext::lock() {
  if (!compileRunning()) {
    return;
  }

  if (gil_lock_depth_ == 0) {
    JIT_DCHECK(
        threaded_compile_tstate_ != nullptr,
        "should only be used on worker threads");
    JIT_DCHECK(PyThreadState_GetUnchecked() == nullptr, "should not have GIL");
    // Acquire GIL by restoring thread-local worker threadstate.
    PyEval_RestoreThread(threaded_compile_tstate_);
  }

  gil_lock_depth_++;
}

void ThreadedCompileContext::unlock() {
  if (!compileRunning()) {
    return;
  }

  gil_lock_depth_--;
  if (gil_lock_depth_ == 0) {
    PyThreadState* save = PyEval_SaveThread();
    JIT_DCHECK(
        threaded_compile_tstate_ == save, "should have saved the same thread");
  }
}

void ThreadedCompileContext::beginWorker(PyThreadState* tstate) {
  JIT_DCHECK(
      threaded_compile_tstate_ == nullptr,
      "should not have saved thread state");
  JIT_DCHECK(current_ == nullptr, "shouldn't have an existing context");
  current_ = this;
  threaded_compile_tstate_ = tstate;
}

void ThreadedCompileContext::endWorker() {
  threaded_compile_tstate_ = nullptr;
  current_ = nullptr;
}

ThreadedCompileContext* ThreadedCompileContext::current() {
  return current_;
}

} // namespace cinderx::jit
