// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/bump_arena.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/inline_cache.h"

#include <vector>

namespace cinderx::jit {

using InlineCacheStats = std::vector<CacheStats>;

struct InlineCacheSite {
  enum class Kind {
    kLoadAttr,
    kLoadMethod,
    kLoadModuleAttr,
    kLoadModuleMethod,
    kLoadTypeAttr,
    kLoadTypeMethod,
    kBinaryOp,
    kStoreAttr,
  };

  InlineCacheSite(BCOffset bytecode_offset, LoadAttrCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, LoadMethodCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, LoadModuleAttrCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, LoadModuleMethodCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, LoadTypeAttrCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, LoadTypeMethodCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, BinaryOpCache* cache);
  InlineCacheSite(BCOffset bytecode_offset, StoreAttrCache* cache);

  Kind kind;
  BCOffset bytecode_offset;
  union {
    LoadAttrCache* load_attr;
    LoadMethodCache* load_method;
    LoadModuleAttrCache* load_module_attr;
    LoadModuleMethodCache* load_module_method;
    LoadTypeAttrCache* load_type_attr;
    LoadTypeMethodCache* load_type_method;
    BinaryOpCache* binary_op;
    StoreAttrCache* store_attr;
  } cache{};
};

class PerCompilationInlineCacheStorage final {
 public:
  const std::vector<InlineCacheSite>& inlineCacheSites() const;

  LoadAttrCache* allocateLoadAttrCache(BCOffset bytecode_offset);
  LoadMethodCache* allocateLoadMethodCache(BCOffset bytecode_offset);
  LoadModuleAttrCache* allocateLoadModuleAttrCache(BCOffset bytecode_offset);
  LoadModuleMethodCache* allocateLoadModuleMethodCache(
      BCOffset bytecode_offset);
  LoadTypeAttrCache* allocateLoadTypeAttrCache();
  LoadTypeAttrCache* allocateLoadTypeAttrCache(BCOffset bytecode_offset);
  LoadTypeMethodCache* allocateLoadTypeMethodCache();
  LoadTypeMethodCache* allocateLoadTypeMethodCache(BCOffset bytecode_offset);
  BinaryOpCache* allocateBinaryOpCache(
      BCOffset bytecode_offset,
      hir::BinaryOpKind op);
  StoreAttrCache* allocateStoreAttrCache(BCOffset bytecode_offset);

  void addLoadTypeAttrCacheSite(
      BCOffset bytecode_offset,
      LoadTypeAttrCache* cache);
  void addLoadTypeMethodCacheSite(
      BCOffset bytecode_offset,
      LoadTypeMethodCache* cache);

 private:
  void addInlineCacheSite(InlineCacheSite site);

  BumpArena inline_cache_arena_;
  std::vector<InlineCacheSite> inline_cache_sites_;
};

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
  BinaryOpCache* allocateBinaryOpCache(
      BCOffset bytecode_offset,
      hir::BinaryOpKind op);
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
  SlabArena<BinaryOpCache> binary_op_caches_;
  SlabArena<StoreAttrCache, AttributeCacheSizeTrait> store_attr_caches_;
};

using InlineCacheStorage = ContextInlineCacheStorage;

} // namespace cinderx::jit
