// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/define.h"

#include <mutex>

namespace cinderx::jit {

// Dedicated lock for protecting JIT compilation data structures such as
// compiled_funcs_, compiled_codes_, active_compiles_, completed_compiles_, etc.
// This is a simple mutex, unlike ThreadedCompileGILHolder which has additional
// logic for GIL handling during threaded compiles.
std::recursive_mutex& jitCompilationMutex();

// pthread_atfork() handlers for the compilation lock.  Compile threads hold it
// with the GIL released, so a child forked at the wrong moment would otherwise
// inherit it locked by a thread that no longer exists.
void jitCompilationAtForkPrepare();
void jitCompilationAtForkParent();
void jitCompilationAtForkChild();

// Free-threaded builds can enter top-level JIT operations concurrently:
// function/code registration, compilation, and destruction hooks.
// Use a dedicated lock instead of ThreadedCompileGILHolder which is a nop
// in GIL disabled builds.
std::recursive_mutex& freeThreadedJITEntrypointMutex();
inline thread_local size_t freeThreadedJITEntrypointLockDepth = 0;
void freeThreadedJITEntrypointAtForkPrepare();
void freeThreadedJITEntrypointAtForkParent();
void freeThreadedJITEntrypointAtForkChild();

class FreeThreadedJITEntrypointGuard {
 public:
  FreeThreadedJITEntrypointGuard() {
    if constexpr (kFreeThreadedBuild) {
      auto& mutex = freeThreadedJITEntrypointMutex();
      if (!mutex.try_lock()) {
        // Detach while waiting so we don't block stop-the-world pauses.
#if PY_VERSION_HEX >= 0x030E0000
        auto* tstate = PyThreadState_GetUnchecked();
#else
        auto* tstate = _PyThreadState_UncheckedGet();
#endif
        if (tstate != nullptr) {
          tstate = PyEval_SaveThread();
        }
        mutex.lock();
        if (tstate != nullptr) {
          PyEval_RestoreThread(tstate);
        }
      }
      ++freeThreadedJITEntrypointLockDepth;
    }
  }

  ~FreeThreadedJITEntrypointGuard() {
    if constexpr (kFreeThreadedBuild) {
      --freeThreadedJITEntrypointLockDepth;
      freeThreadedJITEntrypointMutex().unlock();
    }
  }

  FreeThreadedJITEntrypointGuard(const FreeThreadedJITEntrypointGuard&) =
      delete;
  FreeThreadedJITEntrypointGuard& operator=(
      const FreeThreadedJITEntrypointGuard&) = delete;
  FreeThreadedJITEntrypointGuard(FreeThreadedJITEntrypointGuard&&) = delete;
  FreeThreadedJITEntrypointGuard& operator=(FreeThreadedJITEntrypointGuard&&) =
      delete;
};

// Uses to track if the current thread holds the lock for assertion purposes.
inline thread_local int jitCompilationLockDepth = 0;

class JITCompilationLock {
 public:
  JITCompilationLock() {
    jitCompilationMutex().lock();
    ++jitCompilationLockDepth;
  }

  ~JITCompilationLock() {
    --jitCompilationLockDepth;
    jitCompilationMutex().unlock();
  }

  // Checks if the current thread holds the lock, should only be
  // used for assertions.
  static bool isHeld() {
    return jitCompilationLockDepth > 0;
  }

  JITCompilationLock(const JITCompilationLock&) = delete;
  JITCompilationLock& operator=(const JITCompilationLock&) = delete;
  JITCompilationLock(JITCompilationLock&&) = delete;
  JITCompilationLock& operator=(JITCompilationLock&&) = delete;
};

} // namespace cinderx::jit
