// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/stable_storage.h"

#include <utility>

namespace cinderx::jit {

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadAttrCache* cache_ptr)
    : kind{Kind::kLoadAttr}, bytecode_offset{bytecode_offset} {
  cache.load_attr = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadMethodCache* cache_ptr)
    : kind{Kind::kLoadMethod}, bytecode_offset{bytecode_offset} {
  cache.load_method = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadModuleAttrCache* cache_ptr)
    : kind{Kind::kLoadModuleAttr}, bytecode_offset{bytecode_offset} {
  cache.load_module_attr = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadModuleMethodCache* cache_ptr)
    : kind{Kind::kLoadModuleMethod}, bytecode_offset{bytecode_offset} {
  cache.load_module_method = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadTypeAttrCache* cache_ptr)
    : kind{Kind::kLoadTypeAttr}, bytecode_offset{bytecode_offset} {
  cache.load_type_attr = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    LoadTypeMethodCache* cache_ptr)
    : kind{Kind::kLoadTypeMethod}, bytecode_offset{bytecode_offset} {
  cache.load_type_method = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    BinaryOpCache* cache_ptr)
    : kind{Kind::kBinaryOp}, bytecode_offset{bytecode_offset} {
  cache.binary_op = cache_ptr;
}

InlineCacheSite::InlineCacheSite(
    BCOffset bytecode_offset,
    StoreAttrCache* cache_ptr)
    : kind{Kind::kStoreAttr}, bytecode_offset{bytecode_offset} {
  cache.store_attr = cache_ptr;
}

const std::vector<InlineCacheSite>&
PerCompilationStableStorage::inlineCacheSites() const {
  return inline_cache_sites_;
}

LoadAttrCache* PerCompilationStableStorage::allocateLoadAttrCache(
    BCOffset bytecode_offset) {
  auto cache = arena_.allocate<LoadAttrCache, AttributeCacheSizeTrait>();
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

LoadMethodCache* PerCompilationStableStorage::allocateLoadMethodCache(
    BCOffset bytecode_offset) {
  auto cache = arena_.allocate<LoadMethodCache>();
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

LoadModuleAttrCache* PerCompilationStableStorage::allocateLoadModuleAttrCache(
    BCOffset bytecode_offset) {
  auto cache = arena_.allocate<LoadModuleAttrCache>();
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

LoadModuleMethodCache*
PerCompilationStableStorage::allocateLoadModuleMethodCache(
    BCOffset bytecode_offset) {
  auto cache = arena_.allocate<LoadModuleMethodCache>();
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

LoadTypeAttrCache* PerCompilationStableStorage::allocateLoadTypeAttrCache(
    BCOffset bytecode_offset) {
  auto cache = allocateLoadTypeAttrCache();
  addLoadTypeAttrCacheSite(bytecode_offset, cache);
  return cache;
}

LoadTypeAttrCache* PerCompilationStableStorage::allocateLoadTypeAttrCache() {
  return arena_.allocate<LoadTypeAttrCache>();
}

LoadTypeMethodCache* PerCompilationStableStorage::allocateLoadTypeMethodCache(
    BCOffset bytecode_offset) {
  auto cache = allocateLoadTypeMethodCache();
  addLoadTypeMethodCacheSite(bytecode_offset, cache);
  return cache;
}

LoadTypeMethodCache*
PerCompilationStableStorage::allocateLoadTypeMethodCache() {
  return arena_.allocate<LoadTypeMethodCache>();
}

BinaryOpCache* PerCompilationStableStorage::allocateBinaryOpCache(
    BCOffset bytecode_offset,
    hir::BinaryOpKind op) {
  auto cache = arena_.allocate<BinaryOpCache>(op);
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

StoreAttrCache* PerCompilationStableStorage::allocateStoreAttrCache(
    BCOffset bytecode_offset) {
  auto cache = arena_.allocate<StoreAttrCache, AttributeCacheSizeTrait>();
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
  return cache;
}

void PerCompilationStableStorage::addLoadTypeAttrCacheSite(
    BCOffset bytecode_offset,
    LoadTypeAttrCache* cache) {
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
}

void PerCompilationStableStorage::addLoadTypeMethodCacheSite(
    BCOffset bytecode_offset,
    LoadTypeMethodCache* cache) {
  addInlineCacheSite(InlineCacheSite{bytecode_offset, cache});
}

void PerCompilationStableStorage::addInlineCacheSite(InlineCacheSite site) {
  inline_cache_sites_.emplace_back(std::move(site));
}

LoadAttrCache* ContextStableStorage::allocateLoadAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_attr_caches_.allocate();
}

LoadTypeAttrCache* ContextStableStorage::allocateLoadTypeAttrCache() {
  return load_type_attr_caches_.allocate();
}

LoadTypeAttrCache* ContextStableStorage::allocateLoadTypeAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return allocateLoadTypeAttrCache();
}

LoadMethodCache* ContextStableStorage::allocateLoadMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_method_caches_.allocate();
}

LoadModuleAttrCache* ContextStableStorage::allocateLoadModuleAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_module_attr_caches_.allocate();
}

LoadModuleMethodCache* ContextStableStorage::allocateLoadModuleMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return load_module_method_caches_.allocate();
}

LoadTypeMethodCache* ContextStableStorage::allocateLoadTypeMethodCache() {
  return load_type_method_caches_.allocate();
}

LoadTypeMethodCache* ContextStableStorage::allocateLoadTypeMethodCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return allocateLoadTypeMethodCache();
}

BinaryOpCache* ContextStableStorage::allocateBinaryOpCache(
    [[maybe_unused]] BCOffset bytecode_offset,
    hir::BinaryOpKind op) {
  return binary_op_caches_.allocate(op);
}

StoreAttrCache* ContextStableStorage::allocateStoreAttrCache(
    [[maybe_unused]] BCOffset bytecode_offset) {
  return store_attr_caches_.allocate();
}

void ContextStableStorage::addLoadTypeAttrCacheSite(
    [[maybe_unused]] BCOffset bytecode_offset,
    [[maybe_unused]] LoadTypeAttrCache* cache) {}

void ContextStableStorage::addLoadTypeMethodCacheSite(
    [[maybe_unused]] BCOffset bytecode_offset,
    [[maybe_unused]] LoadTypeMethodCache* cache) {}

InlineCacheStats ContextStableStorage::getAndClearLoadMethodCacheStats() {
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

InlineCacheStats ContextStableStorage::getAndClearLoadTypeMethodCacheStats() {
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
