// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace cinderx {

// Bump allocator that packs sub-allocations into large (2MB) chunks backed by
// transparent huge pages, reducing TLB pressure. Small allocations (e.g. JIT
// slabs) share a chunk instead of each getting its own mapping. Chunks are
// owned by the arena and freed together when it is destroyed. Thread-safe.
class HugePageArena {
 public:
  // 2MB transparent huge pages.
  static constexpr size_t kHugePageSize = 2 * 1024 * 1024;

  HugePageArena() = default;
  ~HugePageArena();

  HugePageArena(const HugePageArena&) = delete;
  HugePageArena& operator=(const HugePageArena&) = delete;

  // Each arena owns its own mutex, so the moved-from arena keeps its lock and
  // is left empty rather than transferring it.
  HugePageArena(HugePageArena&& other) noexcept;

  // Return `size` bytes aligned to at least `alignment`. The memory is owned by
  // the arena and must not be freed by the caller.
  void* allocate(size_t size, size_t alignment);

  // Re-establish huge page backing for every chunk after a fork().
  void afterForkChild();

 private:
  struct Chunk {
    void* ptr;
    size_t size;
  };

  void allocateChunk(size_t size, size_t alignment);

  std::mutex mutex_;
  char* fill_{nullptr};
  char* end_{nullptr};
  std::vector<Chunk> chunks_;
};

// Maintain the pages on huge pages
void hugePagesAfterFork();

} // namespace cinderx
