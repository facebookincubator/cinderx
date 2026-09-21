// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/fork_support.h"

#include "cinderx/Common/define.h"

#if CINDER_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

#include <new>

namespace cinderx {

void destroyMutexMetadataBeforeReinit([[maybe_unused]] std::mutex& mutex) {
#if CINDER_TSAN_ENABLED
  void* native_mutex = mutex.native_handle();
  __tsan_mutex_pre_unlock(native_mutex, 0);
  __tsan_mutex_post_unlock(native_mutex, 0);
  __tsan_mutex_destroy(native_mutex, 0);
#endif
}

void createMutexMetadataAfterReinit([[maybe_unused]] std::mutex& mutex) {
#if CINDER_TSAN_ENABLED
  __tsan_mutex_create(mutex.native_handle(), 0);
#endif
}

void resetMutexAfterFork(std::mutex& mutex) {
  destroyMutexMetadataBeforeReinit(mutex);

  new (&mutex) std::mutex{};

  createMutexMetadataAfterReinit(mutex);
}

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

void resetMutexAfterFork(std::shared_mutex& mutex) {
#if CINDER_TSAN_ENABLED && !defined(_LIBCPP_VERSION)
  // libc++ implements std::shared_mutex using an internal mutex and condition
  // variables rather than a native rwlock, so it does not provide
  // native_handle() and TSAN tracks its internal primitives directly.
  // libstdc++ uses a native pthread_rwlock_t, so we annotate its native handle.
  void* native_mutex = mutex.native_handle();
  __tsan_mutex_pre_unlock(native_mutex, 0);
  __tsan_mutex_post_unlock(native_mutex, 0);
  __tsan_mutex_destroy(native_mutex, 0);
#endif

  new (&mutex) std::shared_mutex{};

#if CINDER_TSAN_ENABLED && !defined(_LIBCPP_VERSION)
  __tsan_mutex_create(mutex.native_handle(), 0);
#endif
}

} // namespace cinderx
