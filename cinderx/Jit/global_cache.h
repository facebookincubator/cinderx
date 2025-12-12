// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus

#include "cinderx/Common/ref.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/Jit/global_cache_iface.h"

#include <set>
#include <unordered_map>

namespace jit {

// Identifies a cached global Python value.
struct GlobalCacheKey {
  // builtins and globals are weak references; the invalidation code is
  // responsible for erasing any relevant keys when a dict is freed.
  BorrowedRef<PyDictObject> builtins;
  BorrowedRef<PyDictObject> globals;
  Ref<PyUnicodeObject> name;

  GlobalCacheKey(
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyUnicodeObject> name);

  ~GlobalCacheKey();

  bool operator==(const GlobalCacheKey& other) const = default;
};

struct GlobalCacheKeyHash {
  std::size_t operator()(const GlobalCacheKey& key) const;
};

using GlobalCacheMap =
    std::unordered_map<GlobalCacheKey, PyObject**, GlobalCacheKeyHash>;

// Wrapper class to initialize, update, and disable a global cache. The actual
// cache lives in a GlobalCacheMap.
class GlobalCache {
 public:
  explicit GlobalCache(GlobalCacheMap::value_type* pair);

  // Get the key for the cached global value.
  const GlobalCacheKey& key() const;

  // Get the address of the cached Python value.
  PyObject** valuePtr() const;

  // Set the global cache pointer.
  void init(PyObject** cache) const;

  // Clear the cache's value.  Unsubscribing from any watched dicts is left to
  // the caller since it can involve complicated dances with iterators.
  void clear();

  bool operator<(const GlobalCache& other) const;

 private:
  GlobalCacheMap::value_type* pair_;
};

// Manages all memory and data structures for global cache values.
class GlobalCacheManager : public IGlobalCacheManager {
 public:
  ~GlobalCacheManager() override;

  // Create or look up a cache for the global with the given name, in the
  // context of the given globals and builtins dicts.  The cache will fall back
  // to builtins if the value isn't defined in the globals dict.
  //
  // The global that is pointed to will automatically be updated as builtins and
  // globals change.  The value that is pointed to will be nullptr if the
  // dictionaries can no longer be tracked or if the value is no longer defined,
  // in which case the dictionaries need to be consulted.  This will return
  // nullptr if the required tracking cannot be initialized.
  PyObject** getGlobalCache(
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyUnicodeObject> key) override;

  // Called when the value at a key is modified (value will contain the new
  // value) or deleted (value will be nullptr).
  void notifyDictUpdate(
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<PyUnicodeObject> key,
      BorrowedRef<> value) override;

  // Called when a dict is cleared, rather than sending individual notifications
  // for every key. The dict is still in a watched state, and further callbacks
  // for it will be invoked as appropriate.
  void notifyDictClear(BorrowedRef<PyDictObject> dict) override;

  // Called when a dict has changed in a way that is incompatible with watching,
  // or is about to be freed.  No more callbacks will be invoked for this dict.
  void notifyDictUnwatch(BorrowedRef<PyDictObject> dict) override;

  // Clear internal caches for global values.  This may cause a degradation of
  // performance and is intended for detecting memory leaks and general cleanup.
  void clear() override;

 private:
  GlobalCache findGlobalCache(
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyUnicodeObject> key);

  // Check if a given key of a dict is watched by the given cache.
  bool isWatchedDictKey(
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<PyUnicodeObject> key,
      GlobalCache cache);

  // Watch the given key of the given dict. updateCache() will be called when
  // the key's value in the dict is changed or removed.  disableCache() will be
  // called if the dict becomes unwatchable.
  void watchDictKey(
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<PyUnicodeObject> key,
      GlobalCache cache);

  // Unsubscribe from the given key of the given dict.
  void unwatchDictKey(
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<PyUnicodeObject> key,
      GlobalCache cache);

  // Initialize a global value cache. Subscribe to both globals and builtins
  // dicts and fill in the current value.
  void initCache(GlobalCache cache);

  // Update the cached value after an update to one of the dicts.
  //
  // Return true iff the cache should be disabled because its builtins dict is
  // unwatchable and the value has been deleted from the globals dict.  The
  // caller is responsible for safely disabling any such caches.
  [[nodiscard]] bool updateCache(
      GlobalCache cache,
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<> new_value);

  // Forget given cache(s). Note that for now, this only removes bookkeeping,
  // each cache is not freed from the arena and may still be reachable from
  // compiled code.
  void disableCaches(const std::vector<GlobalCache>& caches);
  void disableCache(GlobalCache cache);

  // Arena where all the global value caches are allocated.
  SlabArena<PyObject*> arena_;

  // Map of all global value caches, keyed by (globals, builtins, name).
  GlobalCacheMap map_;

  // Two-level map keeping track of which global value caches are subscribed to
  // which keys in which dicts.
  std::unordered_map<
      BorrowedRef<PyDictObject>,
      std::unordered_map<BorrowedRef<PyUnicodeObject>, std::set<GlobalCache>>>
      watch_map_;
};

} // namespace jit

#endif
