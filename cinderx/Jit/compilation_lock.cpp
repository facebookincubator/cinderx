// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compilation_lock.h"

#include "cinderx/Common/define.h"

#if CINDER_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

#include <new>

namespace cinderx::jit {

namespace {

void resetMutexAfterFork(std::recursive_mutex& mutex) {
#if CINDER_TSAN_ENABLED
  void* native_mutex = mutex.native_handle();
  __tsan_mutex_pre_unlock(native_mutex, __tsan_mutex_recursive_unlock);
  __tsan_mutex_post_unlock(native_mutex, 0);
  __tsan_mutex_destroy(native_mutex, 0);
#endif

  new (&mutex) std::recursive_mutex{};

#if CINDER_TSAN_ENABLED
  __tsan_mutex_create(mutex.native_handle(), __tsan_mutex_write_reentrant);
#endif
}

} // namespace

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
  resetMutexAfterFork(jitCompilationMutex());
}

} // namespace cinderx::jit
