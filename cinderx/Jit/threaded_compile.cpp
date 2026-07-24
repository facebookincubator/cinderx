// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/threaded_compile.h"

namespace cinderx::jit {

namespace {

ThreadedCompileContext s_threaded_compile_context;

} // namespace

ThreadedCompileContext& getThreadedCompileContext() {
  return s_threaded_compile_context;
}

void ThreadedCompileContext::startCompile(WorkList&& work_list) {
  // Can't use JIT_CHECK because this is included by log.h.
  assert(!compile_running_);
  work_list_ = std::move(work_list);
  compile_running_ = true;
  interpreter_ = PyInterpreterState_Get();
  tstate_ = PyThreadState_Get();
}

ThreadedCompileContext::WorkList ThreadedCompileContext::endCompile() {
  assert(compile_running_);
  compile_running_ = false;
  return std::move(retry_list_);
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
  return getThreadedCompileContext().compile_running_;
}

bool ThreadedCompileContext::canAccessSharedData() {
  return !compileRunning() ||
      getThreadedCompileContext().holder() == std::this_thread::get_id();
}

PyInterpreterState* ThreadedCompileContext::interpreter() {
  if (compileRunning()) {
    return getThreadedCompileContext().interpreter_;
  }
  return PyInterpreterState_Get();
}

PyThreadState* ThreadedCompileContext::tstate() {
  if (compileRunning()) {
    return getThreadedCompileContext().tstate_;
  }
  return PyThreadState_Get();
}

void ThreadedCompileContext::lock() {
  if (!compileRunning()) {
    return;
  }

  mutex_.lock();

  auto us = std::this_thread::get_id();

  auto prev_level = mutex_recursion_.fetch_add(1, std::memory_order_relaxed);
  if (prev_level == 0) {
    assert(holder() == std::thread::id{});
  } else {
    assert(holder() == us);
  }

  setHolder(us);
}

void ThreadedCompileContext::unlock() {
  if (!compileRunning()) {
    return;
  }

  auto prev_level = mutex_recursion_.fetch_sub(1, std::memory_order_relaxed);
  if (prev_level == 1) {
    setHolder(std::thread::id{});
  } else {
    assert(holder() == std::this_thread::get_id());
  }

  mutex_.unlock();
}

std::thread::id ThreadedCompileContext::holder() const {
  return mutex_holder_.load(std::memory_order_relaxed);
}

void ThreadedCompileContext::setHolder(std::thread::id holder) {
  mutex_holder_.store(holder, std::memory_order_relaxed);
}

} // namespace cinderx::jit
