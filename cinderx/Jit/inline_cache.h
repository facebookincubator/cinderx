// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <array>
#include <memory>
#include <span>
#include <unordered_map>

namespace jit {

// Mutator for an instance attribute that is stored in a split dictionary
struct SplitMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* getAttrKnownOffset(PyObject* obj, PyObject* name);
  int setAttrKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInline(PyObject* obj, PyObject* name);
  PyObject* getAttrSlowPath(
      PyObject* obj,
      PyObject* name,
      BorrowedRef<PyDictObject> dict);
  int setAttrInline(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInlineKnownOffset(PyObject* obj, PyObject* name);
  int setAttrInlineKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
#endif
  bool canInsertToSplitDict(BorrowedRef<PyDictObject> dict, BorrowedRef<> name);
  bool ensureValueOffset(BorrowedRef<> name);

#if PY_VERSION_HEX < 0x030C0000
  uint32_t dict_offset;
  int32_t val_offset;
#else
  Py_ssize_t val_offset;
#endif
  PyDictKeysObject* keys; // Borrowed
};

// Mutator for an instance attribute that is stored in a combined dictionary
struct CombinedMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  Py_ssize_t dict_offset;
};

// Mutator for a data descriptor
struct DataDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  BorrowedRef<> descr;
};

// Mutator for a member descriptor
struct MemberDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  PyMemberDef* memberdef;
};

// Attribute corresponds to a non-data descriptor or a class variable
struct DescrOrClassVarMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  BorrowedRef<> descr;
  uint keys_version;
};

// An instance of AttributeMutator is specialized to more efficiently perform a
// get/set of a particular kind of attribute.
class AttributeMutator {
 public:
  // Kind enum is designed to fit within 3 bits and it's value is embedded into
  // the type_ pointer
  enum class Kind : uint8_t {
    kSplit,
    kSplitKnownOffset,
    kSplitInline,
    kSplitInlineKnownOffset,
    kCombined,
    kDataDescr,
    kMemberDescr,
    kDescrOrClassVar,
    kMaxValue,
  };
  static_assert(
      static_cast<uint8_t>(Kind::kMaxValue) <= 8,
      "Kind enum should fit in 3 bits");

  AttributeMutator();
  PyTypeObject* type() const;
  void reset();
  bool isEmpty() const;
  void set_combined(PyTypeObject* type);
  void set_data_descr(PyTypeObject* type, PyObject* descr);
  void set_member_descr(PyTypeObject* type, PyObject* descr);
  void
  set_descr_or_classvar(PyTypeObject* type, PyObject* descr, uint keys_version);
  void set_split(
      PyTypeObject* type,
      Py_ssize_t val_offset,
      PyDictKeysObject* keys,
      bool values_inline);

  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  static void changeKindFromSplitInline(SplitMutator* split, Kind new_kind);

 private:
  void set_type(PyTypeObject* type, Kind kind);
  Kind get_kind() const;

  uintptr_t type_; // This value stores both a PyTypeObject* for the type object
                   // and the Kind enum value which are bitpacked together to
                   // reduce memory consumption
  union {
    SplitMutator split_;
    CombinedMutator combined_;
    DataDescrMutator data_descr_;
    MemberDescrMutator member_descr_;
    DescrOrClassVarMutator descr_or_cvar_;
  };
};

class AttributeCache {
 public:
  AttributeCache();
  ~AttributeCache();

  void typeChanged(PyTypeObject* type);

 protected:
  std::span<AttributeMutator> entries();

  AttributeMutator* findEmptyEntry();

  void fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name);

  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name, BorrowedRef<> descr);

  AttributeMutator entries_[0];
};

struct AttributeCacheSizeTrait {
  static size_t size() {
    auto base = sizeof(AttributeCache);
    auto extra = sizeof(AttributeMutator) * getConfig().attr_cache_size;
    return base + extra;
  }
};

// A cache for an individual StoreAttrCached instruction.
//
// The logic of StoreAttrCache::invoke is equivalent to PyObject_SetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class StoreAttrCache : public AttributeCache {
 public:
  StoreAttrCache() = default;

  // Return 0 on success and a negative value on failure.
  static int
  invoke(StoreAttrCache* cache, PyObject* obj, PyObject* name, PyObject* value);

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreAttrCache);

  int doInvoke(PyObject* obj, PyObject* name, PyObject* value);
  int invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value);
};

// A cache for an individual LoadAttrCached instruction.
//
// The logic of LoadAttrCache::invoke is equivalent to PyObject_GetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class LoadAttrCache : public AttributeCache {
 public:
  LoadAttrCache() = default;

  // Returns a new reference to the value or NULL on error.
  static PyObject* invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name);

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadAttrCache);

  PyObject* doInvoke(PyObject* obj, PyObject* name);
  PyObject* invokeSlowPath(PyObject* obj, PyObject* name);
};

// A cache for LoadAttr instructions where we expect the receiver to be a type
// object.
//
// The code for loading an attribute where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// second element (the cached value) is loaded. If they are not equal,
// `invoke()` is called, which performs the full lookup and potentially fills
// the cache.
class LoadTypeAttrCache {
 public:
  LoadTypeAttrCache();
  ~LoadTypeAttrCache();

  static PyObject*
  invoke(LoadTypeAttrCache* cache, PyObject* obj, PyObject* name);

  // Get the addresses of the type and value cache entries.
  PyTypeObject** typeAddr();
  PyObject** valueAddr();

  void typeChanged(BorrowedRef<PyTypeObject> type);

 private:
  PyObject* invokeSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  void fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value);
  void reset();

  // Cached type and value, stored as raw pointers so codegen can access them by
  // address.
  PyTypeObject* type_;
  PyObject* value_;
};

#define FOREACH_CACHE_MISS_REASON(V) \
  V(WrongTpGetAttro)                 \
  V(PyDescrIsData)                   \
  V(Uncategorized)

enum class CacheMissReason {
#define DECLARE_CACHE_MISS_REASON(name) k##name,
  FOREACH_CACHE_MISS_REASON(DECLARE_CACHE_MISS_REASON)
#undef DECLARE_CACHE_MISS_REASON
};

std::string_view cacheMissReason(CacheMissReason reason);

struct CacheMiss {
  int count{0};
  CacheMissReason reason{CacheMissReason::kUncategorized};
};

struct CacheStats {
  std::string filename;
  std::string method_name;
  std::unordered_map<std::string, CacheMiss> misses;
};

class LoadMethodCache {
 public:
  struct Entry {
    BorrowedRef<PyTypeObject> type;
    BorrowedRef<> value;
#if PY_VERSION_HEX >= 0x030C0000
    uint keys_version;
#endif

    bool isValidKeysVersion(BorrowedRef<> obj);
  };

  ~LoadMethodCache();

  static LoadMethodResult
  lookupHelper(LoadMethodCache* cache, BorrowedRef<> obj, BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  void typeChanged(PyTypeObject* type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, BorrowedRef<> name);

  std::array<Entry, 4> entries_;
  std::unique_ptr<CacheStats> cache_stats_;
};

// A cache for LoadMethodCached instructions where we expect the receiver to be
// a type object.
//
// The first entry in `entry` is the type receiver. The second entry in `entry`
// is the cached value.
//
// The code for loading a method where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// `getValueHelper()` is called which returns the cached value. If they are not
// equal, `lookupHelper()` is called, which performs the full lookup and
// potentially fills the cache.
class LoadTypeMethodCache {
 public:
  ~LoadTypeMethodCache();

  static LoadMethodResult
  lookupHelper(LoadTypeMethodCache* cache, PyTypeObject* obj, PyObject* name);

  static LoadMethodResult getValueHelper(
      LoadTypeMethodCache* cache,
      PyObject* obj);

  LoadMethodResult lookup(BorrowedRef<PyTypeObject> obj, BorrowedRef<> name);

  // Get the address of the cached type object.
  PyTypeObject** typeAddr();

  // Get the cached method value.
  BorrowedRef<> value();

  void typeChanged(BorrowedRef<PyTypeObject> type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, bool is_bound_meth);

  // Borrowed, but uses a raw pointer as typeAddr() will return the address of
  // this field for codegen purposes.
  PyTypeObject* type_;
  BorrowedRef<> value_;
  std::unique_ptr<CacheStats> cache_stats_;
  bool is_unbound_meth_;
};

// A cache for an individual LoadModuleAttrCached instruction.
class LoadModuleAttrCache {
 public:
  static PyObject* lookupHelper(
      LoadModuleAttrCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  PyObject* lookup(BorrowedRef<> obj, BorrowedRef<> name);

 private:
  PyObject* lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<> obj, BorrowedRef<> value, ci_dict_version_tag_t version);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t version_{0};
  BorrowedRef<> value_;
#endif
};

class LoadModuleMethodCache {
 public:
  static LoadMethodResult lookupHelper(
      LoadModuleMethodCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  BorrowedRef<> moduleObj();
#if PY_VERSION_HEX < 0x030E0000
  BorrowedRef<> value();
#else
  PyObject** cache() {
    return cache_;
  }
#endif

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_obj_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t module_version_{0};
  BorrowedRef<> value_;
#endif
};

// Invalidate all load/store attr caches for type
void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type);

} // namespace jit

struct FunctionEntryCacheValue {
  void** ptr{nullptr};
  Ref<_PyTypedArgsInfo> arg_info;
};

using FunctionEntryCacheMap =
    jit::UnorderedMap<PyFunctionObject*, FunctionEntryCacheValue>;
