// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compilation_lock.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cinderx::jit {

// Threaded-compile state. Stack-allocated in pyjit.cpp when a multi-threaded
// or background compile starts. Its address is published via thread-local
// storage so workers can find it.
class ThreadedCompileContext {
 public:
  // Used for background compilation (single background thread, no work list).
  ThreadedCompileContext();

  virtual ~ThreadedCompileContext();
  ThreadedCompileContext(const ThreadedCompileContext&) = delete;
  ThreadedCompileContext(ThreadedCompileContext&&) = delete;
  ThreadedCompileContext& operator=(const ThreadedCompileContext&) = delete;
  ThreadedCompileContext& operator=(ThreadedCompileContext&&) = delete;

  // Stop the current iteration of the multi-threaded compile, and returns
  // true if the compile was ended or false if it was previously ended.
  bool endCompile();

  // Check if the current thread is currently participating in a multi-threaded
  // or background compile.
  static bool compileRunning();

  // Worker-side: set TLS to indicate we are inside a worker with a saved
  // tstate.
  void beginWorker(PyThreadState* tstate);
  void endWorker();

  void releaseGil();

  // Returns true if it's safe for the current thread to access data protected
  // by the threaded compile lock, either because no threaded compile is active
  // or the current thread holds the GIL via ThreadedCompileGILHolder.
  static bool canAccessSharedData();

  static PyInterpreterState* interpreter();
  static PyThreadState* tstate();

  static ThreadedCompileContext* current();

 private:
  friend class ThreadedCompileGILHolder;

  static void lock();
  static void unlock();

  // The interpreter state that kicked off the multi-threaded compile.
  PyInterpreterState* interpreter_{nullptr};

  static thread_local ThreadedCompileContext* current_;
  // Thread-local state for GIL-based serialization.
  // threaded_compile_tstate_ holds the PyThreadState* that was saved when the
  // GIL was released. When non-null, the thread is running without the GIL and
  // threaded_compile_tstate_ is the threadstate needed to restore (acquire) the
  // GIL via PyEval_RestoreThread. gil_lock_depth_ tracks recursion of the
  // GIL-based lock.
  static thread_local PyThreadState* threaded_compile_tstate_;
  static thread_local int gil_lock_depth_;

  // Saved main thread state when GIL is released (batch compile).
  PyThreadState* main_{nullptr};
  bool ended_{false};
};

// RAII device for acquiring the GIL when doing multi-threaded compilation.
class ThreadedCompileGILHolder {
 public:
  ThreadedCompileGILHolder() {
    ThreadedCompileContext::lock();
  }

  ~ThreadedCompileGILHolder() {
    ThreadedCompileContext::unlock();
  }

  ThreadedCompileGILHolder(const ThreadedCompileGILHolder&) = delete;
  ThreadedCompileGILHolder(ThreadedCompileGILHolder&&) = delete;
  ThreadedCompileGILHolder& operator=(const ThreadedCompileGILHolder&) = delete;
  ThreadedCompileGILHolder& operator=(ThreadedCompileGILHolder&&) = delete;
};

} // namespace cinderx::jit
