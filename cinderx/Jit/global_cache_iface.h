// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace jit {

class IGlobalCacheManager {
 public:
  IGlobalCacheManager() {}
  virtual ~IGlobalCacheManager() = default;

  // Create or look up a cache for the global with the given name, in the
  // context of the given globals and builtins dicts.  The cache will fall back
  // to builtins if the value isn't defined in the globals dict.
  virtual PyObject** getGlobalCache(
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyUnicodeObject> key) = 0;

  // Called when the value at a key is modified (value will contain the new
  // value) or deleted (value will be nullptr).
  virtual void notifyDictUpdate(
      BorrowedRef<PyDictObject> dict,
      BorrowedRef<PyUnicodeObject> key,
      BorrowedRef<> value) = 0;

  // Called when a dict is cleared, rather than sending individual notifications
  // for every key. The dict is still in a watched state, and further callbacks
  // for it will be invoked as appropriate.
  virtual void notifyDictClear(BorrowedRef<PyDictObject> dict) = 0;

  // Called when a dict has changed in a way that is incompatible with watching,
  // or is about to be freed.  No more callbacks will be invoked for this dict.
  virtual void notifyDictUnwatch(BorrowedRef<PyDictObject> dict) = 0;

  // Clear internal caches for global values.  This may cause a degradation of
  // performance and is intended for detecting memory leaks and general cleanup.
  virtual void clear() = 0;
};

} // namespace jit
