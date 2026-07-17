// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/slab_arena.h"

#include "cinderx/module_state.h"

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

} // namespace cinderx
