// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/fork_support.h"

#include "cinderx/Common/define.h"

#if CINDER_TSAN_ENABLED
#include <sanitizer/tsan_interface.h>
#endif

#include <new>

namespace cinderx {

void resetMutexAfterFork(std::mutex& mutex) {
#if CINDER_TSAN_ENABLED
  void* native_mutex = mutex.native_handle();
  __tsan_mutex_pre_unlock(native_mutex, 0);
  __tsan_mutex_post_unlock(native_mutex, 0);
  __tsan_mutex_destroy(native_mutex, 0);
#endif

  new (&mutex) std::mutex{};

#if CINDER_TSAN_ENABLED
  __tsan_mutex_create(mutex.native_handle(), 0);
#endif
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

} // namespace cinderx
