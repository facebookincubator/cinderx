// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/bump_arena.h"

#include "cinderx/Common/util.h"

#include <algorithm>
#include <cstdint>

namespace cinderx {

namespace {

constexpr size_t kAlign = size_t{kPageSize};
constexpr size_t kMaxBlockSize = size_t{kMiB};

} // namespace

BumpArena::~BumpArena() {
  for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
    it->destroy(it->obj);
  }
}

void* BumpArena::allocateBytes(size_t size, size_t alignment) {
  JIT_DCHECK(isPowerOfTwo(alignment), "Alignment must be a power of 2");

  Block* block;
  size_t offset;

  if (blocks_.empty()) {
    block = &addBlock(size, alignment);
    offset = 0;
  } else {
    block = &blocks_.back();

    const auto base = reinterpret_cast<uintptr_t>(block->base.get());
    offset = roundUp(base + block->fill, alignment) - base;

    if (offset > block->size || size > block->size - offset) {
      block = &addBlock(size, alignment);
      offset = 0;

      JIT_DCHECK(offset <= block->size, "BumpArena block alignment too large");
      JIT_DCHECK(size <= block->size - offset, "BumpArena block too small");
    }
  }

  void* ptr = block->base.get() + offset;
  block->fill = offset + size;
  return ptr;
}

BumpArena::Block& BumpArena::addBlock(size_t min_size, size_t alignment) {
  const size_t size = roundUp(std::max(min_size, next_block_size_), kAlign);
  next_block_size_ = std::min(next_block_size_ * 2, kMaxBlockSize);

  Block& block = blocks_.emplace_back(
      Block{AlignedMemory<char>{size, std::max(alignment, kAlign)}, 0, size});

  return block;
}

} // namespace cinderx
