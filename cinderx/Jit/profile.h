// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/inline_cache.h"

#include <algorithm>
#include <compare>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

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

// The various types of entries that can be contained in a profile. Entries
// hold borrowed references so they can be recorded in HIR and in the
// collector's secondary entries without the GIL; the type is kept alive by
// the originating profile during compilation and by CodeRuntime references
// afterwards, and snapshot() takes owning references for the new profile's
// owned set with the GIL held.
using ProfileEntry = std::variant<BinaryOpProfile, LoadAttrProfile>;

// Invokes func once with each Python object borrowed by entry that must be
// kept alive while the entry is in use. Entries hold borrowed references so
// they can be recorded without the GIL; the caller owns taking a strong
// reference, e.g. into Profile's owned set or into the CodeRuntime.
template <typename F>
void collectProfileEntryRefs(const ProfileEntry& entry, F&& func) {
  if (const auto* load_attr = std::get_if<LoadAttrProfile>(&entry)) {
    if (load_attr->receiver_type != nullptr) {
      func(BorrowedRef<>(load_attr->receiver_type.getObj()));
    }
  }
}

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
  void add(BorrowedRef<PyCodeObject> code, BCOffset offset, CacheT* cache) {
#ifndef ENABLE_PREFORK_MODEL
    entries_[ProfileLocation(code, offset)].emplace_back(cache);
#endif
  }
  // Record a profile used in a re-compile where we're not necessarily
  // emitting an inline cache.
  void add(ProfileLocation location, ProfileEntry payload);
  // Snapshot the observed inline-cache states. Must be called with the GIL
  // held: profiling LoadAttr caches takes strong references to the observed
  // receiver types.
  Profile snapshot() const;

 private:
#ifndef ENABLE_PREFORK_MODEL
  std::unordered_map<ProfileLocation, std::vector<ProfileSource>> entries_;
  // Secondary entries: payloads carried forward from the previous profile.
  // A location may be reached more than once (e.g. the same bytecode offset
  // via multiple inlined ProfileUseTypes), so each location accumulates
  // every carried payload; snapshot() merges them with the same semantics
  // as live caches.
  std::unordered_map<ProfileLocation, std::vector<ProfileEntry>>
      secondary_entries_;
#endif
};

} // namespace cinderx::jit
