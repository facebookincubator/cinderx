// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/aligned_memory.h"
#include "cinderx/Common/slab_arena.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace cinderx {

// A mixed-type bump allocator that keeps allocated addresses stable and
// destroys non-trivially-destructible objects when the arena is destroyed.
class BumpArena {
 public:
  BumpArena() = default;
  ~BumpArena();

  BumpArena(const BumpArena&) = delete;
  BumpArena& operator=(const BumpArena&) = delete;
  BumpArena(BumpArena&&) = delete;
  BumpArena& operator=(BumpArena&&) = delete;

  template <
      typename T,
      typename SizeTrait = ObjectSizeTrait<T>,
      typename... Args>
  T* allocate(Args&&... args) {
    std::lock_guard<std::mutex> guard{mutex_};

    const size_t size = SizeTrait::size();
    JIT_CHECK(size >= sizeof(T), "SizeTrait must allocate enough space");

    void* mem = allocateBytes(size, alignof(T));
    T* obj = new (mem) T(std::forward<Args>(args)...);

    /* If there is a destructor, we are going to track it here so that when the
     * whole arena is freed we can call it. */
    if constexpr (!std::is_trivially_destructible_v<T>) {
      static_assert(std::is_nothrow_destructible_v<T>);
      destructors_.push_back(Destructor{obj, [](void* ptr) noexcept {
                                          std::destroy_at(static_cast<T*>(ptr));
                                        }});
    }

    return obj;
  }

 private:
  struct Block {
    AlignedMemory<char> base;
    size_t fill{0};
    size_t size{0};
  };

  struct Destructor {
    void* obj;
    void (*destroy)(void*) noexcept;
  };

  void* allocateBytes(size_t size, size_t alignment);
  Block& addBlock(size_t min_size, size_t alignment);

  std::vector<Block> blocks_;
  std::vector<Destructor> destructors_;
  size_t next_block_size_{size_t{kPageSize}};
  std::mutex mutex_;
};

} // namespace cinderx
