// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compilation_lock.h"

#include <new>

namespace cinderx::jit {

std::recursive_mutex& jitCompilationMutex() {
  static std::recursive_mutex mutex;
  return mutex;
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
  new (&jitCompilationMutex()) std::recursive_mutex{};
}

} // namespace cinderx::jit
