// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>

#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

namespace cinderx::jit {

// Apple Silicon maps JIT memory with MAP_JIT, which for any given thread is
// either writable or executable but never both.  Every write into that memory
// has to be bracketed by these two -- both the code allocator laying down
// freshly generated code, and anything that overwrites code afterwards, like a
// CodePatcher.
//
// Both are no-ops elsewhere: other platforms map the region RWX.

inline void jitEnableWriting() {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(0);
#endif
}

inline void jitEnableExecuting(
    [[maybe_unused]] void* addr,
    [[maybe_unused]] size_t size) {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(addr, size);
#endif
}

} // namespace cinderx::jit
