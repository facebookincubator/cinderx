// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/slab_arena.h"

#include "cinderx/Common/fork_support.h"
#include "cinderx/module_state.h"

#include <algorithm>

#if defined(__linux__) && defined(__aarch64__)
// On ARM64 we see huge dTLB misses on our inline caches so
// we put them on huge pages
#define ALLOCATE_HUGE_PAGES
#endif

namespace cinderx {

std::shared_ptr<HugePageArena> getSharedHugePageArena() {
#ifdef ALLOCATE_HUGE_PAGES
  auto state = getModuleState();
  if (state != nullptr) {
    return state->getSharedHugePageArena();
  }
#endif
  return nullptr;
}

SlabArenaForkRegistry& SlabArenaForkRegistry::get() {
  // Deliberately leaked: SlabArenas are owned by the CinderX module state,
  // which can outlive static destructors and would then unregister into a
  // destroyed object.
  static auto* registry = new SlabArenaForkRegistry;
  return *registry;
}

void SlabArenaForkRegistry::add(std::mutex* mutex) {
  std::lock_guard<std::mutex> guard{lock_};
  mutexes_.push_back(mutex);
}

void SlabArenaForkRegistry::remove(std::mutex* mutex) {
  auto it = std::find(mutexes_.begin(), mutexes_.end(), mutex);
  if (it != mutexes_.end()) {
    *it = std::move(mutexes_.back());
    mutexes_.pop_back();
  }
}

void SlabArenaForkRegistry::atForkPrepare() {
  // Holding lock_ across the fork also stops the list itself from being
  // mutated while it's being walked.
  lock_.lock();
  for (std::mutex* mutex : mutexes_) {
    mutex->lock();
  }
}

void SlabArenaForkRegistry::atForkParent() {
  for (std::mutex* mutex : mutexes_) {
    mutex->unlock();
  }
  lock_.unlock();
}

void SlabArenaForkRegistry::atForkChild() {
  // Reuse each mutex's storage to get a fresh, unlocked one.  They're all
  // still locked by atForkPrepare() and destroying a locked mutex is
  // undefined, so their lifetimes end without running their destructors.
  for (std::mutex* mutex : mutexes_) {
    resetMutexAfterFork(*mutex);
  }
  resetMutexAfterFork(lock_);
}

} // namespace cinderx
