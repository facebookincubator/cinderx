// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/inline_cache.h"

#include <algorithm>
#include <compare>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace cinderx::jit {

// The key we use to identify a profile entry in a profile. Currently this
// is just the code and offset which allows accounting for functions inlined
// on a previous compile but doesn't allow us to account for the same function
// being inlined in different positions with different types.
struct ProfileLocation {
  BorrowedRef<PyCodeObject> code;
  BCOffset bytecode_offset;

  auto operator<=>(const ProfileLocation&) const = default;
};

} // namespace cinderx::jit

template <>
struct std::hash<cinderx::jit::ProfileLocation> {
  size_t operator()(const cinderx::jit::ProfileLocation& loc) const noexcept {
    std::hash<cinderx::BorrowedRef<PyCodeObject>> code_hasher;
    std::hash<cinderx::jit::BCOffset> offset_hasher;
    return cinderx::combineHash(
        code_hasher(loc.code), offset_hasher(loc.bytecode_offset));
  }
};

namespace cinderx::jit {

struct BinaryOpProfile {
  hir::Type left_type, right_type, res_type;

  bool operator==(const BinaryOpProfile&) const = default;
};

// Profile feedback from a LoadAttr inline cache.
struct LoadAttrProfile {
  // Borrowed reference to the receiver type, present only when the cache
  // observed a single receiver type. The owning reference lives in Profile's
  // owned set, keeping the type alive until the recompilation consumes the
  // profile.
  BorrowedRef<PyTypeObject> receiver_type;
  // Kind of attribute access observed, present only when the cache observed a
  // single entry.
  std::optional<AttributeMutator::Kind> kind;
  // True when the cache observed more than one entry.
  bool polymorphic{false};

  bool operator==(const LoadAttrProfile&) const = default;
};

// The various types of entries that can be contained in a profile
using ProfileEntry = std::variant<BinaryOpProfile, LoadAttrProfile>;

// A snapshot of the profiling information for a CompiledFunction that can
// be used during re-compilation.
struct Profile {
  Profile() = default;

  explicit Profile(
      std::unordered_map<ProfileLocation, ProfileEntry>&& entries,
      std::unordered_set<Ref<>>&& owned_refs = {})
      : entries_(std::move(entries)), owned_refs_(std::move(owned_refs)) {}

  template <typename T>
  const T* find(const ProfileLocation& location) const {
    auto code_entries = entries_.find(location);
    if (code_entries == entries_.end()) {
      return nullptr;
    }
    return std::get_if<T>(&code_entries->second);
  }

  template <typename T>
  const T* find(BorrowedRef<PyCodeObject> code, BCOffset offset) const {
    return find<T>(ProfileLocation{code, offset});
  }

  const std::unordered_map<ProfileLocation, ProfileEntry>& entries() const {
    return entries_;
  }

 private:
  std::unordered_map<ProfileLocation, ProfileEntry> entries_;
  // Owning references for the borrowed pointers in entries_. Entries hold
  // BorrowedRefs so they can be copied without the GIL during compilation;
  // this set keeps the referents alive until the recompilation consumes the
  // profile.
  std::unordered_set<Ref<>> owned_refs_;
};

using ProfileSource = std::variant<BinaryOpCache*, LoadAttrCache*>;

// Snapshots the profiling information for a CompiledFunction. The collector
// gets seeded with the caches associated with a CompiledFunction during
// compilation and gets stored with the CompiledFunctionData. When we re-compile
// this will extract all of the collected profiling information to enable
// further specialization.
class ProfileCollector {
 public:
  // Register a live inline cache for a location. Templated over the cache
  // type so new cache kinds only extend ProfileSource; the variant
  // conversion below rejects anything else.
  template <typename CacheT>
  void add(BorrowedRef<PyCodeObject> location, BCOffset offset, CacheT* cache) {
#ifndef ENABLE_PREFORK_MODEL
    entries_[ProfileLocation(location, offset)].emplace_back(cache);
#endif
  }
  // Snapshot the observed inline-cache states. Must be called with the GIL
  // held: profiling LoadAttr caches takes strong references to the observed
  // receiver types.
  Profile snapshot() const;

 private:
#ifndef ENABLE_PREFORK_MODEL
  std::unordered_map<ProfileLocation, std::vector<ProfileSource>> entries_;
#endif
};

} // namespace cinderx::jit
