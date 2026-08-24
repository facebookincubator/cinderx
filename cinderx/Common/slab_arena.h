// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/hugepages.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/slab.h"
#include "cinderx/Common/util.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace cinderx {

template <class T>
struct ObjectSizeTrait {
  static constexpr size_t size() {
    return roundUp(sizeof(T), alignof(T));
  }
};

template <typename T, size_t kSlabSize>
class SlabArenaIterator {
 public:
  SlabArenaIterator() = default;

  explicit SlabArenaIterator(std::vector<Slab<T, kSlabSize>>* slabs)
      : slabs_{slabs} {
    if (slabs_ == nullptr) {
      return;
    }
    JIT_CHECK(slabs_->size() > 0, "Unexpected empty slabs list");
    slab_ = slabs_->begin();
    slab_iter_ = currentSlab().begin();
    if (isSlabEnd()) {
      *this = SlabArenaIterator{};
    }
  }

  bool operator==(const SlabArenaIterator& other) const = default;

  T& operator*() {
    return *slab_iter_;
  }

  const T& operator*() const {
    return *slab_iter_;
  }

  SlabArenaIterator& operator++() {
    slab_iter_++;
    if (isSlabEnd()) {
      slab_++;
      if (isArenaEnd()) {
        return *this = SlabArenaIterator{};
      }
      slab_iter_ = currentSlab().begin();
      // Only the last slab can be empty: a new slab is appended only when the
      // previous one is full, and an empty slab accepts the next allocation.
      if (isSlabEnd()) {
        return *this = SlabArenaIterator{};
      }
    }
    return *this;
  }

  SlabArenaIterator operator++(int) {
    auto ret = *this;
    operator++();
    return ret;
  }

 private:
  bool isArenaEnd() const {
    return slabs_ == nullptr || slab_ == slabs_->end();
  }

  bool isSlabEnd() const {
    return isArenaEnd() || slab_iter_ == slab_->end();
  }

  Slab<T, kSlabSize>& currentSlab() const {
    return *slab_;
  }

  // Store a slab list, iterator to a slab within that list, and an iterator to
  // a position within that slab. Past-the-end and uninitialized iterators will
  // contain all value-initialized members.
  std::vector<Slab<T, kSlabSize>>* slabs_{nullptr};
  typename std::vector<Slab<T, kSlabSize>>::iterator slab_{};
  SlabIterator<T> slab_iter_;
};

std::shared_ptr<HugePageArena> getSharedHugePageArena();

// The mutexes of every live SlabArena, so that pthread_atfork() handlers can
// quiesce them across a fork().
//
// SlabArena is a template with instances scattered across JIT state, so they
// register themselves here instead of being enumerated by hand.  No SlabArena
// ever locks another, so the handlers may take them in any order.
class SlabArenaForkRegistry {
 public:
  static SlabArenaForkRegistry& get();

  void add(std::mutex* mutex);
  void remove(std::mutex* mutex);

  void atForkPrepare();
  void atForkParent();
  void atForkChild();

 private:
  std::mutex lock_;
  std::vector<std::mutex*> mutexes_;
};

// SlabArena is a simple arena allocator, using slabs that are multiples of the
// system's page size. Allocated objects never move after creation, and all
// objects will be kept alive until the SlabArena they came from is destroyed.
//
// It is intended to keep objects of a given type together on the same page,
// either to achieve desired certain copy-on-write behavior, or to mlock() all
// of the objects with minimal collateral damage (which can be managed with
// SlabArena::mlock() and SlabArena::munlock()).
//
// allocate(), mlock(), and munlock() are thread-safe. begin(), end(), and all
// operations on SlabArena::iterator are not thread-safe.
template <
    typename T,
    typename SizeTrait = ObjectSizeTrait<T>,
    size_t kPagesPerSlab = 4>
class SlabArena {
  static constexpr size_t kSlabSize = kPageSize * kPagesPerSlab;
  static_assert(
      sizeof(T) <= kSlabSize,
      "Cannot allocate objects larger than one slab");

 public:
  using iterator = SlabArenaIterator<T, kSlabSize>;

  SlabArena() {
    slabs_.emplace_back(SizeTrait::size(), getSharedHugePageArena());
    // Registered last so a throwing constructor can't leave a dangling pointer
    // behind, as the destructor won't run for a half-constructed arena.
    SlabArenaForkRegistry::get().add(&mutex_);
  }

  ~SlabArena() {
    SlabArenaForkRegistry::get().remove(&mutex_);
  }

  SlabArena(const SlabArena&) = delete;
  SlabArena(SlabArena&&) = delete;
  SlabArena& operator=(const SlabArena&) = delete;
  SlabArena& operator=(SlabArena&&) = delete;

  // Allocate a new instance of T using the given constructor arguments.
  template <typename... Args>
  T* allocate(Args&&... args) {
    std::lock_guard<std::mutex> guard{mutex_};

#ifndef WIN32
    if (mlocked_) {
      // It's not necessarily an error to allocate after locking but it's
      // probably not what we expect to happen in the common forking case.
      JIT_DLOG("Allocating while locked");
    }
#endif

    T* object = slabs_.back().emplace(std::forward<Args>(args)...);
    if (object == nullptr) {
      auto& slab =
          slabs_.emplace_back(SizeTrait::size(), getSharedHugePageArena());
#ifndef WIN32
      if (mlocked_) {
        slab.mlock();
      }
#endif
      object = slab.emplace(std::forward<Args>(args)...);
      JIT_CHECK(object != nullptr, "Empty slab failed to allocate");
    }
    return object;
  }

#ifndef WIN32
  // Pin the contents to physical memory.
  void mlock() {
    std::lock_guard<std::mutex> guard{mutex_};
    for (auto& slab : slabs_) {
      slab.mlock();
    }
  }

  // Unpin the contents from physical memory.
  void munlock() {
    std::lock_guard<std::mutex> guard{mutex_};
    for (auto& slab : slabs_) {
      slab.munlock();
    }
  }
#endif

  iterator begin() {
    return iterator{&slabs_};
  }

  iterator end() {
    return iterator{};
  }

 private:
  std::vector<Slab<T, kSlabSize>> slabs_;
  std::mutex mutex_;
#ifndef WIN32
  bool mlocked_{false};
#endif
};

} // namespace cinderx
