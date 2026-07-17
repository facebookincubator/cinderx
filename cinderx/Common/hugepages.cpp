// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/hugepages.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#ifndef WIN32
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifndef WIN32
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif
#endif

namespace cinderx {

HugePageArena::HugePageArena(HugePageArena&& other) noexcept {
  std::lock_guard<std::mutex> lock{other.mutex_};
  fill_ = other.fill_;
  end_ = other.end_;
  chunks_ = std::move(other.chunks_);
  other.fill_ = nullptr;
  other.end_ = nullptr;
}

HugePageArena::~HugePageArena() {
  for (const Chunk& chunk : chunks_) {
    free_aligned(chunk.ptr);
  }
}

void* HugePageArena::allocate(size_t size, size_t alignment) {
  std::lock_guard<std::mutex> lock{mutex_};

  char* candidate = fill_ == nullptr
      ? nullptr
      : reinterpret_cast<char*>(
            roundUp(reinterpret_cast<uintptr_t>(fill_), alignment));
  if (candidate == nullptr || candidate + size > end_) {
    // Current chunk is exhausted (or none exists yet). A fresh chunk is aligned
    // to at least `alignment` and sized to fit, so the allocation succeeds.
    allocateChunk(size, alignment);
    candidate = reinterpret_cast<char*>(
        roundUp(reinterpret_cast<uintptr_t>(fill_), alignment));
  }
  fill_ = candidate + size;
  return candidate;
}

void HugePageArena::allocateChunk(size_t size, size_t alignment) {
  size_t chunk_alignment = std::max(alignment, kHugePageSize);
  size_t chunk_size = roundUp(size, kHugePageSize);
  void* chunk = malloc_aligned(chunk_size, chunk_alignment);
  JIT_CHECK(chunk != nullptr, "Failed to allocate {} bytes", chunk_size);
#ifdef MADV_HUGEPAGE
  // Advise the kernel to back the chunk with transparent huge pages.
  madvise(chunk, chunk_size, MADV_HUGEPAGE);
#endif
  chunks_.emplace_back(chunk, chunk_size);
  fill_ = static_cast<char*>(chunk);
  end_ = fill_ + chunk_size;
}

void HugePageArena::afterForkChild() {
#ifndef WIN32
  void* tmp = nullptr;
  size_t tmp_size = 0;
  std::lock_guard<std::mutex> lock{mutex_};
  for (const Chunk& chunk : chunks_) {
    // we can theoretically have chunks that are larger than 2MB but don't
    // really
    if (tmp == nullptr || tmp_size < chunk.size) {
      tmp = realloc(tmp, chunk.size);
      tmp_size = chunk.size;
      JIT_CHECK(tmp != nullptr, "Failed to allocate {} bytes", chunk.size);
    }

    // Fault every page in so the child gets its own private physical pages
    // instead of copy-on-write references to the parent. A volatile write
    // forces the fault without the compiler optimizing it away.
    memcpy(tmp, chunk.ptr, chunk.size);
    if (madvise(chunk.ptr, chunk.size, MADV_DONTNEED) != 0) {
      JIT_DLOG(
          "CINDERX: MADV_DONTNEED failed for {} bytes at {} after fork: {}\n",
          chunk.size,
          chunk.ptr,
          strerror(errno));
    }
    if (madvise(chunk.ptr, chunk.size, MADV_HUGEPAGE) != 0) {
      JIT_DLOG(
          "CINDERX: MADV_HUGEPAGE failed for {} bytes at {} after fork: {}\n",
          chunk.size,
          chunk.ptr,
          strerror(errno));
    }
    memcpy(chunk.ptr, tmp, chunk.size);
  }
  free(tmp);
#endif
}

} // namespace cinderx
