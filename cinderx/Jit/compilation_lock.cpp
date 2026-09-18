// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compilation_lock.h"

#include "cinderx/Common/fork_support.h"

namespace cinderx::jit {

std::recursive_mutex& jitCompilationMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

std::recursive_mutex& freeThreadedJITEntrypointMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

void freeThreadedJITEntrypointAtForkPrepare() {
  if constexpr (kFreeThreadedBuild) {
    freeThreadedJITEntrypointMutex().lock();
  }
}

void freeThreadedJITEntrypointAtForkParent() {
  if constexpr (kFreeThreadedBuild) {
    freeThreadedJITEntrypointMutex().unlock();
  }
}

void freeThreadedJITEntrypointAtForkChild() {
  if constexpr (kFreeThreadedBuild) {
    // Other threads disappear at fork. Reinit the mutex and restore only
    // the surviving thread's recursion depth.
    auto& mutex = freeThreadedJITEntrypointMutex();
    resetMutexAfterFork(mutex);
    for (size_t i = 0; i < freeThreadedJITEntrypointLockDepth; ++i) {
      mutex.lock();
    }
  }
}

void jitCompilationAtForkPrepare() {
  jitCompilationMutex().lock();
}

void jitCompilationAtForkParent() {
  jitCompilationMutex().unlock();
}

void jitCompilationAtForkChild() {
  // Reuse the storage to get a fresh, unlocked mutex.  Unlocking is not an
  // option even though the forking thread is the owner: a recursive mutex
  // identifies its owner by thread id, and the surviving thread gets a new one
  // across the fork, so unlock() would fail with EPERM and leave the lock held
  // forever.  Destroying it isn't an option either, as it is still locked by
  // atForkPrepare(), so its lifetime ends without running its destructor.
  resetMutexAfterFork(jitCompilationMutex());
}

} // namespace cinderx::jit
