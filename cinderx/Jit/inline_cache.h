// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/containers.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cinderx::jit::hir {
// Defined in cinderx/Jit/hir/hir.h; only the complete type is needed in
// inline_cache.cpp, so a forward declaration suffices here.
enum class BinaryOpKind;
} // namespace cinderx::jit::hir

namespace cinderx::jit {

// Gates dispatching attribute inline caches through a function pointer stored
// in the cache, instead of a fixed address baked into the generated code.
//
// When enabled, a cache re-points that slot at the narrowest entry point its
// currently *populated* entries allow -- the configured attr_cache_size only
// caps how far the ladder can go:
//
//   0 entries  -> invokeEmpty
//   1 entry    -> specialized<K> for the AttributeMutator::Kind it settled on
//   2, 3, 4    -> invokeUnrolled<N>
//   5 or more  -> invoke, the general scan
#if defined(__aarch64__) || !defined(ENABLE_PREFORK_MODEL)
#define CINDERX_IC_USE_TARGET_PROMOTION
#endif

constexpr bool kInlineCachesTargetPromote =
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
    true;
#else
    false;
#endif

// How many of the packed type pointer's top bits are reserved for
// AttributeMutator::Kind. The field sits above any representable address rather
// than in the low alignment bits, so it is not capped at the 3 bits
// PyTypeObject's 8-byte alignment guarantees.
constexpr unsigned kAttrKindBitCount = 4;
constexpr unsigned kAttrKindLimit = 1u << kAttrKindBitCount;

class LoadAttrCache;
class StoreAttrCache;

// The signatures codegen calls through. The cache is always the last argument,
// leaving the leading argument registers to obj/name/value, which the caller is
// more likely to already have in place.
using LoadAttrTarget = PyObject* (*)(PyObject * obj,
                                     PyObject* name,
                                     LoadAttrCache*);
using StoreAttrTarget =
    int (*)(PyObject* obj, PyObject* name, PyObject* value, StoreAttrCache*);

// Mutator for an instance attribute that is stored in a split dictionary
struct SplitMutator {
  static PyObject* getAttr(PyObject* obj, PyObject* name, SplitMutator* split);
  static int
  setAttr(PyObject* obj, PyObject* name, PyObject* value, SplitMutator* split);
#if PY_VERSION_HEX >= 0x030E0000
  static PyObject*
  getAttrInline(PyObject* obj, PyObject* name, SplitMutator* split);
  static PyObject* getAttrSlowPath(
      PyObject* obj,
      PyObject* name,
      BorrowedRef<PyDictObject> dict);
  static int setAttrInline(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      SplitMutator* split);
#endif
  bool canInsertToSplitDict(BorrowedRef<PyDictObject> dict, BorrowedRef<> name);

  Py_ssize_t val_offset;
  PyDictKeysObject* keys; // Borrowed
};

// Mutator for an instance attribute that is stored in a combined dictionary
// (non-managed-dict types with tp_dictoffset).
struct CombinedMutator {
  static PyObject*
  getAttr(PyObject* obj, PyObject* name, CombinedMutator* combined);
  static int setAttr(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      CombinedMutator* combined);

  Py_ssize_t dict_offset;
  BorrowedRef<> getattr_method;
};

// Mutator for an instance attribute on a managed-dict type where the attribute
// is not in the shared keys (e.g. shared keys are full). Uses the managed dict
// APIs directly rather than a stored dict_offset.
struct DictMutator {
  static PyObject* getAttr(PyObject* obj, PyObject* name, DictMutator* dict);
  static int
  setAttr(PyObject* obj, PyObject* name, PyObject* value, DictMutator* dict);

  BorrowedRef<> getattr_method;
};

// Mutator for a data descriptor
struct DataDescrMutator {
  static PyObject* getAttr(PyObject* obj, DataDescrMutator* data_descr);
  static int
  setAttr(PyObject* obj, PyObject* value, DataDescrMutator* data_descr);

  BorrowedRef<> descr;
  BorrowedRef<PyTypeObject> descr_type;
};

// Mutator for a member descriptor
struct MemberDescrMutator {
  static PyObject* getAttr(PyObject* obj, MemberDescrMutator* member_descr);
  static int
  setAttr(PyObject* obj, PyObject* value, MemberDescrMutator* member_descr);

  PyMemberDef* memberdef;
  BorrowedRef<> getattr_method; // Cached __getattr__ if the type has one
};

// Attribute corresponds to a non-data descriptor or a class variable
struct DescrOrClassVarMutator {
  static PyObject*
  getAttr(PyObject* obj, PyObject* name, DescrOrClassVarMutator* descr_or_cvar);
  static int setAttr(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      DescrOrClassVarMutator* descr_or_cvar);

  BorrowedRef<> descr;
  uint32_t keys_version;
};

// Mutator for attribute lookups on types that define __getattr__.
// Used when a particular attribute name is absent from both the type's MRO
// and the instance dict, causing __getattr__ to be invoked.
struct GetAttrMutator {
  static PyObject*
  getAttr(PyObject* obj, PyObject* name, GetAttrMutator* getattr);

  BorrowedRef<> getattr_method;
  uint32_t keys_version;
};

#ifdef CINDERX_IC_USE_TARGET_PROMOTION
// Mutator for an attribute read off a module.
//
// Unlike every other kind, the enclosing entry's type guard is not enough here:
// it only proves the receiver is *a* module, and the cached value belongs to
// one specific module's dict. So this additionally checks object identity, then
// checks that the dict has not changed under it. A receiver that is a different
// module, or the same module after a mutation, re-populates in place rather
// than giving up on the entry.
//
// Mirrors LoadModuleAttrCache, which does the same job for sites where the
// compiler already knew the receiver was a module. The validity mechanism
// differs by version for the same reason it does there: 3.14+ can hold a
// pointer into the interpreter's global cache and test it in one load, whereas
// 3.12 has to keep the value alongside the dict version it was read at.
struct ModuleMutator {
  static PyObject* getAttr(PyObject* obj, PyObject* name, ModuleMutator* mod);

  BorrowedRef<> module;
  PyObject** cache;
};

// Mutator for an attribute read off a type object, e.g. `SomeClass.attr`.
//
// Unlike every other kind, the enclosing entry's `type_` holds the *receiver*
// -- the class being read -- rather than Py_TYPE(receiver). A class is a
// PyTypeObject*, so this is type-correct, and it is what makes the entry cost
// nothing but the value: `ac_watcher.watch(cls)` plus
// AttributeCache::typeChanged(cls), which resets entries by matching
// entry.type(), is the entire invalidation story. PyType_Modified recurses into
// subclasses and fires the watcher per visited type, so a value inherited from
// a base is invalidated too, even though the entry names only the subclass.
//
// The flip side is that an entry of this kind must never be reached by the
// generic scan, which matches entry.type() against Py_TYPE(obj) and would
// therefore hand an *instance* of the cached class the class's own attribute.
// Nothing does: retarget() monomorphises the cache and points it at
// LoadAttrCache::invokeType, which matches against the receiver itself, and
// getAttrForKind has no kType arm, so any other route aborts rather than
// answering wrongly.
struct TypeMutator {
  // Null until the first invokeType miss populates it. It stays null for a
  // class whose read is not cacheable -- a descriptor that has to run, say --
  // which keeps that class pinned to the slow path instead of retrying an
  // entry it can never fill.
  BorrowedRef<> value;
};

// Mutator for an attribute read off a class that has a metaclass of its own,
// e.g. the `cls.class_var` of a classmethod called on such a class.
//
// The value still comes from the class's own MRO -- the metaclass is not where
// `class_var` lives -- so this is TypeMutator's job with one extra thing to
// prove. type_getattro consults the metatype first and lets a data descriptor
// found there win over the class's MRO, and kType gets that for free by only
// ever claiming classes whose metatype is `type` itself, which is immutable and
// so can be settled once at fill time. A metaclass can grow such a descriptor
// at any point, so an entry here records the metatype's version tag alongside
// the value and re-resolves when it moves.
//
// The metatype is deliberately not watched. Watching is keyed on entry.type(),
// which is the class, and several classes at one site commonly share a
// metaclass -- so a watch would need its own unwatch bookkeeping, in exchange
// for saving a compare that the receiver's header load has already paid for.
struct MetaTypeMutator {
  // Null until the first fill populates it, and for a class whose read is not
  // cacheable. See TypeMutator::value.
  BorrowedRef<> value;

  // Py_TYPE(class)->tp_version_tag as of the read that produced `value`.
  // Version tags are unique across types and are cleared by PyType_Modified, so
  // a match proves both that the metatype is still the one consulted and that
  // nothing has been added to it since.
  uint32_t metatype_version;
};
#endif

// The single source of truth for AttributeMutator::Kind. Everything that has
// to enumerate the kinds -- the enum itself, the getAttr/setAttr dispatch
// switches, and the tables mapping a kind to its specialized entry point -- is
// generated from this list, so a new kind only has to be added here plus given
// a body in getAttrForKind/setAttrForKind.
//
// X(name, store_ok): store_ok is 1 when a *store* can be specialized for the
// kind. __getattr__ only participates in loads, so kGetAttr is load-only and
// the store-side tables skip it rather than emitting a body that can only
// abort.
//
// CINDERX_ATTR_KIND_PROMOTION_ONLY holds the kinds that are only registered
// under target promotion. They include a mix of object-identity versions that
// the non-promotion version can't handle as well as more versions which would
// bloat the switch.
//
// The three *GetAttr kinds are the second half of each descriptor kind: kFoo
// and kFooGetAttr share a sub-mutator and a getAttrForKind body, and differ
// only in whether that body runs the __getattr__ fallback after a failed
// lookup. fill() picks the *GetAttr half exactly when the type has a
// __getattr__ for the fallback to find, so the half every other type gets is
// nothing but the descriptor call. Load-only, since __getattr__ does not
// participate in stores. See baseKind() and runsGetAttrFallback().
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
#define CINDERX_ATTR_KIND_PROMOTION_ONLY(X) \
  X(kDataDescrGetAttr, 0)                   \
  X(kMemberDescrGetAttr, 0)                 \
  X(kDescrOrClassVarGetAttr, 0)             \
  X(kModule, 0)                             \
  X(kType, 0)                               \
  X(kMetaType, 0)
#else
#define CINDERX_ATTR_KIND_PROMOTION_ONLY(X)
#endif

#define CINDERX_FOREACH_ATTR_KIND(X) \
  X(kSplit, 1)                       \
  X(kSplitInline, 1)                 \
  X(kCombined, 1)                    \
  X(kDataDescr, 1)                   \
  X(kMemberDescr, 1)                 \
  X(kDescrOrClassVar, 1)             \
  X(kGetAttr, 0)                     \
  X(kDict, 1)                        \
  CINDERX_ATTR_KIND_PROMOTION_ONLY(X)

// Emit `body` only for kinds a store can specialize for, by pasting on the
// store_ok column above. Used by the store-side generators so they drop
// kGetAttr instead of instantiating a body that could only abort.
//
// `body` may contain commas as long as they are inside parentheses, which is
// true of every call expression it is used with.
#define CINDERX_ATTR_KIND_STORE_ONLY_1(body) body
#define CINDERX_ATTR_KIND_STORE_ONLY_0(body)
#define CINDERX_ATTR_KIND_STORE_ONLY(store_ok, body) \
  CINDERX_ATTR_KIND_STORE_ONLY_##store_ok(body)

// An instance of AttributeMutator is specialized to more efficiently perform a
// get/set of a particular kind of attribute.
class AttributeMutator {
 public:
  // Kind's value is bitpacked into the top kAttrKindBitCount bits of the type_
  // pointer.
  enum class Kind : uint8_t {
#define CINDERX_ATTR_KIND_ENUMERATOR(name, store_ok) name,
    CINDERX_FOREACH_ATTR_KIND(CINDERX_ATTR_KIND_ENUMERATOR)
#undef CINDERX_ATTR_KIND_ENUMERATOR
        kMaxValue,
  };
  static_assert(
      static_cast<uint8_t>(Kind::kMaxValue) <= kAttrKindLimit,
      "Kind enum does not fit in the bits reserved for it in the type pointer");

  // Maps a kind to the one it is a variant of: each descriptor kind's *GetAttr
  // twin maps back to the plain kind, and every other kind maps to itself. A
  // pair shares a sub-mutator, so whatever cares which union member is live --
  // the get dispatch, watchedDescrType -- matches on this rather than on the
  // kind directly.
  static constexpr Kind baseKind(Kind kind) {
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
    switch (kind) {
      case Kind::kDataDescrGetAttr:
        return Kind::kDataDescr;
      case Kind::kMemberDescrGetAttr:
        return Kind::kMemberDescr;
      case Kind::kDescrOrClassVarGetAttr:
        return Kind::kDescrOrClassVar;
      default:
        break;
    }
#endif
    return kind;
  }

  // Whether a descriptor kind's body runs the __getattr__ fallback after a
  // failed lookup. Only the *GetAttr half of a pair does. Without the split
  // there is a single kind per descriptor shape, which has to cover types with
  // and without __getattr__ and so always carries it.
  static constexpr bool runsGetAttrFallback(Kind kind) {
    return !kInlineCachesTargetPromote || kind != baseKind(kind);
  }

  AttributeMutator();
  PyTypeObject* type() const;
  void reset();
  bool isEmpty() const;
  void setCombined(PyTypeObject* type);
  void setDict(PyTypeObject* type);
  // The descriptor kinds take the type's __getattr__ (as getGetAttrForCaching
  // resolves it, so null when the type has none) rather than looking it up
  // themselves: it selects between the kind and its *GetAttr twin, and only the
  // caller knows whether this is a load, which is the only side that has a
  // twin to pick.
  void setDataDescr(PyTypeObject* type, PyObject* descr, BorrowedRef<> getattr);
  void
  setMemberDescr(PyTypeObject* type, PyObject* descr, BorrowedRef<> getattr);
  void setDescrOrClassvar(
      PyTypeObject* type,
      PyObject* descr,
      uint32_t keys_version,
      BorrowedRef<> getattr);
  void setSplit(
      PyTypeObject* type,
      Py_ssize_t val_offset,
      PyDictKeysObject* keys,
      bool values_inline);
  void setGetattr(
      PyTypeObject* type,
      PyObject* getattr_method,
      uint32_t keys_version);
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  // `type` is the module's type, not the module. See ModuleMutator.
  void setModule(PyTypeObject* type, PyObject* module);

  ModuleMutator* asModule() {
    JIT_DCHECK(
        getKind() == AttributeMutator::Kind::kModule,
        "should only be used on modules");
    return &module_;
  }

  // `cls` is the class being read, which goes straight into type_. See
  // TypeMutator.
  void setTypeAttr(PyTypeObject* cls);

  TypeMutator* asTypeAttr() {
    JIT_DCHECK(
        getKind() == AttributeMutator::Kind::kType,
        "should only be used on type attributes");
    return &type_attr_;
  }

  // As with setTypeAttr, `cls` is the class being read. The metatype version is
  // recorded by the fill that produces a value, not here. See
  // MetaTypeMutator.
  void setMetaTypeAttr(PyTypeObject* cls);

  MetaTypeMutator* asMetaTypeAttr() {
    JIT_DCHECK(
        getKind() == AttributeMutator::Kind::kMetaType,
        "should only be used on metaclass-instance type attributes");
    return &type_instance_attr_;
  }

  // Whether `kind` is one of the two keyed on the class being read rather than
  // on Py_TYPE(receiver). They share a monomorphisation family and a slow path
  // -- see AttributeCache::monomorphise and LoadAttrCache::invokeTypeSlow.
  static constexpr bool isTypeAttrKind(Kind kind) {
    return kind == Kind::kType || kind == Kind::kMetaType;
  }

  // The packed type-and-kind word, for a guard that has to establish both in a
  // single compare. See LoadAttrCache::invokeType.
  uintptr_t typeAndKind() const {
    return type_;
  }
#endif

  BorrowedRef<PyTypeObject> watchedDescrType() const;

  static PyObject*
  getAttr(PyObject* obj, PyObject* name, AttributeMutator* entry);
  static int setAttr(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      AttributeMutator* entry);

  // getAttr/setAttr for a Kind that is already known at compile time. Each
  // instantiation inlines exactly the one sub-mutator body it needs, so a
  // caller that has already established the kind pays no switch and no jump
  // table. getAttr/setAttr are themselves written in terms of these, so there
  // is one copy of each body.
  //
  // The caller is responsible for having checked the kind; calling these on a
  // mutator of a different kind reinterprets the union and is undefined.
  template <Kind K>
  static PyObject*
  getAttrForKind(PyObject* obj, PyObject* name, AttributeMutator* entry);
  template <Kind K>
  static int setAttrForKind(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      AttributeMutator* entry);

  // Whether a mutator of this kind can rewrite its own Kind in place, keeping
  // the same cached type. Only kSplitInline can: SplitMutator's inline
  // get/setAttr downgrade it to kSplit when a receiver's inline values go
  // invalid.
  //
  // A Kind-specialized entry point guards on the receiver type alone, which
  // cannot see such a change, so whoever runs a drifting kind's body has to
  // re-derive the dispatch slot afterwards. Everything else is pinned by the
  // type check.
  template <Kind K>
  static constexpr bool kCanDriftInPlace = K == Kind::kSplitInline;

  Kind getKind() const;

  static void changeKindFromSplitInline(SplitMutator* split, Kind new_kind);
  template <typename T>
  static AttributeMutator* from(T* mutator) {
    return reinterpret_cast<AttributeMutator*>(
        reinterpret_cast<uintptr_t>(mutator) -
        offsetof(AttributeMutator, split_));
  }

 private:
  void setType(PyTypeObject* type, Kind kind);

  uintptr_t type_; // This value stores both a PyTypeObject* for the type object
                   // and the Kind enum value which are bitpacked together to
                   // reduce memory consumption
  union {
    SplitMutator split_;
    CombinedMutator combined_;
    DictMutator dict_;
    DataDescrMutator data_descr_;
    MemberDescrMutator member_descr_;
    DescrOrClassVarMutator descr_or_cvar_;
    GetAttrMutator getattr_;
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
    ModuleMutator module_;
    TypeMutator type_attr_;
    MetaTypeMutator type_instance_attr_;
#endif
  };
};

class AttributeCache {
 public:
  AttributeCache();
  ~AttributeCache();

  void typeChanged(PyTypeObject* type);
  void descrTypeChanged(PyTypeObject* type);

 protected:
  std::span<AttributeMutator> entries();

  AttributeMutator* findEmptyEntry();

  AttributeMutator* fill(BorrowedRef<> obj, BorrowedRef<> name, bool is_set);

#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  // Dispatch slot. Codegen loads it by absolute address (targetAddr()) and
  // calls through it.
  //
  // It has to live here in the base rather than in the derived classes, and
  // ahead of entries_. entries_ is a trailing flexible array, which makes
  // sizeof(AttributeCache) zero without this member, so a pointer declared in a
  // derived class would land at offset 0 too and alias entries_[0] -- silently
  // corrupting the first mutator on the first write.
  //
  // Load and store need different signatures, hence the union; each derived
  // class exposes the arm it uses.
  union Target {
    LoadAttrTarget load;
    StoreAttrTarget store;
  };
  Target target_{};
#endif

  // Entry counts at or above this all dispatch to the same general scan, so
  // countEntries() saturates here rather than walking the rest of the array.
  static constexpr unsigned kScanEntryCount = 5;

  // Number of populated entries, saturating at kScanEntryCount. Entries are
  // kept packed at the front of the array, so a result of N means exactly
  // entries_[0 .. N-1] are live.
  unsigned countEntries();

  // Restore that packing after an invalidation punches a hole in the middle,
  // shifting the survivors forward in order and clearing the slots they came
  // from. The unrolled entry points read entries_[0 .. N-1] unconditionally, so
  // they depend on there being no gaps.
  //
  // Moving an entry between slots is safe for the watchers: they are keyed on
  // the AttributeCache*, not on an entry index.
  void packEntries();

  void monomorphise(AttributeMutator::Kind kind);

  // Must stay the last data member as these are dynamically sized based upon
  // the configured cache size.
  AttributeMutator entries_[0];
};

struct AttributeCacheSizeTrait {
  static size_t size() {
    auto base = sizeof(AttributeCache);
    auto extra = sizeof(AttributeMutator) * getConfig().attr_cache_size;
    return base + extra;
  }
};

// A cache for an individual StoreAttr instruction.
//
// The logic of StoreAttrCache::invoke is equivalent to PyObject_SetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class StoreAttrCache : public AttributeCache {
 public:
  StoreAttrCache();

  // Scan every entry for one matching the receiver's type. Return 0 on success
  // and a negative value on failure.
  static int
  invoke(PyObject* obj, PyObject* name, PyObject* value, StoreAttrCache* cache);

  // Entry point for a cache with nothing cached yet. Every call goes straight
  // to the slow path, which installs a Kind-specialized target once the first
  // entry has been filled.
  static int invokeEmpty(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      StoreAttrCache* cache);

  // Entry point for a cache whose one populated entry has kind K. Guards on the
  // receiver type; anything else -- a different type, an invalidated entry, a
  // second type showing up -- falls into the slow path, which re-derives the
  // dispatch slot.
  template <AttributeMutator::Kind K>
  static int specialized(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      StoreAttrCache* cache);

  // Entry point for a cache with exactly N populated entries, for the small N
  // worth unrolling. Entries are packed from index 0, so this walks
  // entries_[0 .. N-1] with the trip count known at compile time -- no loop
  // bookkeeping and no re-reading of the configured size, which is what the
  // general scan in doInvoke pays for.
  template <unsigned N>
  static int invokeUnrolled(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      StoreAttrCache* cache);

  // Address of the dispatch slot, for codegen to load and call through.
  StoreAttrTarget* targetAddr();

  StoreAttrCache(const StoreAttrCache&) = delete;
  StoreAttrCache& operator=(const StoreAttrCache&) = delete;

 private:
  int doInvoke(PyObject* obj, PyObject* name, PyObject* value);
  static int invokeSlowPath(
      PyObject* obj,
      PyObject* name,
      PyObject* value,
      StoreAttrCache* cache);

  // Point the dispatch slot at whatever suits the currently populated entries.
  // Compiles away to nothing when target promotion is disabled, so callers do
  // not have to guard the call.
  void retarget();

  void setTargetAddr(StoreAttrTarget target) {
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
    target_.store = target;
#else
    throw std::runtime_error{"setTargetAddr: not supported"};
#endif
  }

  static StoreAttrTarget targetForKind(AttributeMutator::Kind kind);
};

// A cache for an individual LoadAttr instruction.
//
// The logic of LoadAttrCache::invoke is equivalent to PyObject_GetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class LoadAttrCache : public AttributeCache {
 public:
  LoadAttrCache();

  // Scan every entry for one matching the receiver's type. Returns a new
  // reference to the value or NULL on error.
  static PyObject* invoke(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // See the notes on StoreAttrCache's equivalents.
  static PyObject*
  invokeEmpty(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  template <AttributeMutator::Kind K>
  static PyObject*
  specialized(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // See StoreAttrCache::invokeUnrolled.
  template <unsigned N>
  static PyObject*
  invokeUnrolled(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  static PyObject*
  invokeModule(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // The class-receiver counterpart of invokeModule. Owns a cache that
  // retarget() has monomorphised to class receivers, and matches the leading
  // entry against the receiver itself rather than against Py_TYPE(receiver).
  // See TypeMutator.
  static PyObject*
  invokeType(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // invokeType for a type w/ a meta type.
  static PyObject*
  invokeMetaType(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // Address of the dispatch slot, for codegen to load and call through.
  LoadAttrTarget* targetAddr();

  LoadAttrCache(const LoadAttrCache&) = delete;
  LoadAttrCache& operator=(const LoadAttrCache&) = delete;

 private:
  static PyObject*
  invokeSlowPath(PyObject* obj, PyObject* name, LoadAttrCache* cache);

#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  // The cold half of both class-receiver entry points: anything the leading
  // entry did not claim -- a second class, a first read of one, an entry whose
  // guard no longer holds, or a receiver that is not a class at all. Kept out
  // of line so the hit path is a straight line, in the same shape as
  // invoke/invokeSlowPath.
  static PyObject*
  invokeTypeSlow(PyObject* obj, PyObject* name, LoadAttrCache* cache);

  // Point the dispatch slot at whichever class-receiver entry point matches the
  // leading entry's kind, which is the only entry either of them reads.
  void setTypeTarget();
#endif

  // See the note on StoreAttrCache::retarget.
  void retarget(AttributeMutator* mut);

  void setTargetAddr(LoadAttrTarget target) {
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
    target_.load = target;
#else
    throw std::runtime_error{"setTargetAddr: not supported"};
#endif
  }

  static LoadAttrTarget targetForKind(AttributeMutator::Kind kind);
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
    // Borrowed cached attribute, tagged in the low bit. When the low bit is
    // clear the value is an untagged PyObject* for a bound method (the common,
    // hot case) -- a single bit test selects it and it is bound to the
    // receiver. When the low bit is set the value is not a bound method:
    //   * value == 1 (just the tag bit): the attribute is absent from the type
    //     and must be resolved via __getattr__ / __getattribute__ dispatch.
    //   * value > 1: a tagged PyObject* for a staticmethod descriptor or class
    //     variable; untag it and return it as a plain attribute (no self
    //     binding) -- unless is_class_method is set (see below).
    // Use the tag helpers in inline_cache.cpp to read it.
    uintptr_t value{0};
    uint32_t keys_version;

    // For a NULL sentinel entry (value == nullptr), records whether the type
    // dispatches a genuine miss to __getattr__ (true) or has a lookup we can't
    // replicate, e.g. a custom __getattribute__ (false). Meaningless when
    // value != nullptr.
    bool has_getattr_hook{false};

    // Set when the entry caches a class method (a Python-level classmethod or a
    // C-level classmethod_descriptor). The (tagged, unbound) value is the
    // underlying callable, which lookup() binds to the receiver's type rather
    // than to the receiver itself. Only meaningful for unbound entries.
    bool is_class_method{false};

    bool isValidKeysVersion(BorrowedRef<> obj);
  };
  static_assert(sizeof(Entry) == 24, "Entry must be small");

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
  void fill(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<> value,
      BorrowedRef<> name,
      bool has_getattr_hook,
      bool is_bound_method = true,
      bool is_class_method = false);

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

  BorrowedRef<> module_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
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

  BorrowedRef<> module_obj_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  ci_dict_version_tag_t module_version_{0};
  BorrowedRef<> value_;
#endif
};

// Identifies a single operand type a SpecializedType expects.  Kept in sync
// with FOREACH_OPERAND_TYPE (inline_cache.cpp): every type there maps to a
// k<Name> value here, enforced at compile time by the
// SpecializedType::k##NAME uses in checkFor.
enum class SpecializedType : uint8_t {
  // The cache has not specialized yet (still in a populate state).
  kUninitialized,
  // The cache has fallen back to the generic PyNumber_Add/Multiply path.
  kGeneric,
  kCompactLong,
  kLong,
  kUnicode,
  kFloat,
  kList,
  kTuple,
  kComplex,
};

// A cache for an individual BinaryOp instruction.
//
// Implements an inline cache for binary operations as a small state machine.
// A single Specialization enum covers both add and multiply states, but add and
// multiply have separate dispatch entry points (add() / multiply()) that each
// switch over their op's subset of the enum.  A cache is constructed for a
// single op; it starts in that op's populate state, which checks the inputs for
// known cache types on the first invocation, then transitions specialization_
// to the matching specialized state, or to a generic state when no
// SpecializedType applies.
//
// Codegen emits a direct call to add() (for kAdd) or multiply() (for
// kMultiply); each switches on specialization_ and calls the matching
// specialized operation directly -- there is no indirect call through a
// function pointer.
class BinaryOpCache {
 public:
  // Identifies which specialization the cache has settled on, i.e. which
  // operation add()/multiply() dispatches to.  A single enum holds both ops'
  // states: the k<Name> values are auto-generated from
  // FOREACH_BINARY_OP_SPECIALIZATION, the kUninitialized* values are the
  // initial (lazily specializing) populate states, and
  // kAddGeneric/kMultiplyGeneric are the permanent generic fallbacks.  add()
  // only ever observes the add subset and multiply() the multiply subset, but a
  // single enum lets specializedTypes() switch over all values without a
  // discriminant.
  enum class Specialization : uint8_t;

  // The (lhs, rhs, return) operand/result types a cache has specialized to.
  // The return type is tracked when known, which lets a specialization step
  // down to a wider one when the result no longer matches (e.g. a compact int
  // add whose result overflows the compact range).
  struct BinarySpecialization {
    SpecializedType lhs;
    SpecializedType rhs;
    SpecializedType ret;

    bool operator==(const BinarySpecialization&) const = default;
  };

  // Constructs a cache for op, seeding the matching per-op populate state
  // (which specializes lazily on the first call).  Throws std::runtime_error if
  // op has no inline-cache support.
  explicit BinaryOpCache(cinderx::jit::hir::BinaryOpKind op);

  // Dispatch entry points called directly by codegen: add() for kAdd,
  // multiply() for kMultiply.  Each switches on the cache's per-op
  // specialization enum and runs the corresponding operation directly.
  static PyObject* add(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);
  static PyObject* multiply(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  // Returns the (lhs, rhs, return) operand types the cache has settled on
  // ({kUninitialized, ...} before the first call).
  BinarySpecialization specializedTypes() const;

  BinaryOpCache(const BinaryOpCache&) = delete;
  BinaryOpCache& operator=(const BinaryOpCache&) = delete;

 private:
  // Selects the initial populate state for op, or throws std::runtime_error if
  // op is not supported.
  static Specialization selectInitialSpecialization(
      cinderx::jit::hir::BinaryOpKind op);

  // Initial entry point for the add op: inspects the operand types, transitions
  // the add specialization, and performs the operation.
  static PyObject*
  populateAndInvokeAdd(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  // Initial entry point for the multiply op: inspects the operand types,
  // transitions the multiply specialization, and performs the operation.
  static PyObject*
  populateAndInvokeMultiply(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  // Permanent generic fallback that just calls PyNumber_Add.
  static PyObject*
  addGeneric(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  // Permanent generic fallback that just calls PyNumber_Multiply.
  static PyObject*
  multiplyGeneric(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  // Specialized entry for a (lhs, rhs) -> ret triple.  Guards that lhs passes
  // checkFor(LhsKind) and rhs passes checkFor(RhsKind) and, if so, runs the
  // fast-path Op.  When the return type is refined (returnNeedsCheck), it also
  // verifies the result matches checkFor(ReturnKind) and, if not, steps the
  // specialization down to Fallback (a wider specialization) while still
  // returning the already-correct result.  If the operands stop matching, it
  // sets specialization_ to Fallback and re-dispatches through ReDispatch.
  // Fallback is the next Specialization in the chain and ReDispatch is the
  // matching add()/multiply().
  template <
      auto LhsKind,
      auto RhsKind,
      auto ReturnKind,
      auto Op,
      auto Fallback,
      auto ReDispatch>
  static PyObject*
  invokeSpecialized(PyObject* lhs, PyObject* rhs, BinaryOpCache* cache);

  Specialization specialization_;
};

// Invalidate all load/store attr caches for type
void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type);

} // namespace cinderx::jit

struct FunctionEntryCacheValue {
  void** ptr{nullptr};
  cinderx::Ref<_PyTypedArgsInfo> arg_info;
};

using FunctionEntryCacheMap =
    cinderx::UnorderedMap<PyFunctionObject*, FunctionEntryCacheValue>;
