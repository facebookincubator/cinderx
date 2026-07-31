// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/inline_cache_storage.h"

namespace cinderx::jit {

LoadAttrCache* ContextInlineCacheStorage::allocateLoadAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_attr_caches_.allocate();
}

LoadTypeAttrCache* ContextInlineCacheStorage::allocateLoadTypeAttrCache() {
  return load_type_attr_caches_.allocate();
}

LoadTypeAttrCache* ContextInlineCacheStorage::allocateLoadTypeAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return allocateLoadTypeAttrCache();
}

LoadMethodCache* ContextInlineCacheStorage::allocateLoadMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_method_caches_.allocate();
}

LoadModuleAttrCache* ContextInlineCacheStorage::allocateLoadModuleAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_module_attr_caches_.allocate();
}

LoadModuleMethodCache* ContextInlineCacheStorage::allocateLoadModuleMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_module_method_caches_.allocate();
}

LoadTypeMethodCache* ContextInlineCacheStorage::allocateLoadTypeMethodCache() {
  return load_type_method_caches_.allocate();
}

LoadTypeMethodCache* ContextInlineCacheStorage::allocateLoadTypeMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return allocateLoadTypeMethodCache();
}

StoreAttrCache* ContextInlineCacheStorage::allocateStoreAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return store_attr_caches_.allocate();
}

void ContextInlineCacheStorage::addLoadTypeAttrCacheSite(
    [[maybe_unused]] BCOffset bytecode_offset,
    [[maybe_unused]] LoadTypeAttrCache* cache) {}

void ContextInlineCacheStorage::addLoadTypeMethodCacheSite(
    [[maybe_unused]] BCOffset bytecode_offset,
    [[maybe_unused]] LoadTypeMethodCache* cache) {}

InlineCacheStats ContextInlineCacheStorage::getAndClearLoadMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

InlineCacheStats
ContextInlineCacheStorage::getAndClearLoadTypeMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_type_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

} // namespace cinderx::jit
