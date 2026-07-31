// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/slab_arena.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/inline_cache.h"

#include <vector>

namespace cinderx::jit {

using InlineCacheStats = std::vector<CacheStats>;

class ContextInlineCacheStorage final {
 public:
  LoadAttrCache* allocateLoadAttrCache(BCOffset bytecode_offset);
  LoadTypeAttrCache* allocateLoadTypeAttrCache();
  LoadTypeAttrCache* allocateLoadTypeAttrCache(BCOffset bytecode_offset);
  LoadMethodCache* allocateLoadMethodCache(BCOffset bytecode_offset);
  LoadModuleAttrCache* allocateLoadModuleAttrCache(BCOffset bytecode_offset);
  LoadModuleMethodCache* allocateLoadModuleMethodCache(
      BCOffset bytecode_offset);
  LoadTypeMethodCache* allocateLoadTypeMethodCache();
  LoadTypeMethodCache* allocateLoadTypeMethodCache(BCOffset bytecode_offset);
  StoreAttrCache* allocateStoreAttrCache(BCOffset bytecode_offset);
  void addLoadTypeAttrCacheSite(
      BCOffset bytecode_offset,
      LoadTypeAttrCache* cache);
  void addLoadTypeMethodCacheSite(
      BCOffset bytecode_offset,
      LoadTypeMethodCache* cache);

  InlineCacheStats getAndClearLoadMethodCacheStats();
  InlineCacheStats getAndClearLoadTypeMethodCacheStats();

 private:
  SlabArena<LoadAttrCache, AttributeCacheSizeTrait> load_attr_caches_;
  SlabArena<LoadTypeAttrCache> load_type_attr_caches_;
  SlabArena<LoadMethodCache> load_method_caches_;
  SlabArena<LoadModuleAttrCache> load_module_attr_caches_;
  SlabArena<LoadModuleMethodCache> load_module_method_caches_;
  SlabArena<LoadTypeMethodCache> load_type_method_caches_;
  SlabArena<StoreAttrCache, AttributeCacheSizeTrait> store_attr_caches_;
};

using InlineCacheStorage = ContextInlineCacheStorage;

} // namespace cinderx::jit
