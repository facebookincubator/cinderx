// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"

#include <cstddef>
#include <cstdlib>
#include <memory>

#ifdef WIN32
#include <malloc.h>
#endif

namespace cinderx {

struct AlignedMemoryDeleter {
  void operator()(void* ptr) const {
#ifdef WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
  }
};

template <typename T>
class AlignedMemory {
 public:
  AlignedMemory(size_t size, size_t alignment) {
    void* ptr;
#ifdef WIN32
    ptr = _aligned_malloc(size, alignment);
    JIT_CHECK(ptr != nullptr, "Failed to allocate {} bytes", size);
#else
    int result = posix_memalign(&ptr, alignment, size);
    JIT_CHECK(result == 0, "Failed to allocate {} bytes", size);
#endif
    ptr_.reset(static_cast<T*>(ptr));
  }

  T* get() const {
    return ptr_.get();
  }

 private:
  std::unique_ptr<T, AlignedMemoryDeleter> ptr_;
};

} // namespace cinderx
