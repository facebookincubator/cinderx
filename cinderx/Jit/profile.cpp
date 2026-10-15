// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/profile.h"

#include <optional>

using namespace cinderx::jit::hir;

namespace cinderx::jit {

namespace {

std::optional<ProfileEntry> binaryOpPayload(const BinaryOpCache& cache) {
  const BinaryOpCache::BinarySpecialization specialization =
      cache.specializedTypes();
  const std::optional<Type> left_type =
      specializedTypeToHirType(specialization.lhs);
  const std::optional<Type> right_type =
      specializedTypeToHirType(specialization.rhs);
  const std::optional<Type> res = specializedTypeToHirType(specialization.ret);
  if (!left_type.has_value() || !right_type.has_value()) {
    return std::nullopt;
  }
  return BinaryOpProfile{
      *left_type, *right_type, res.has_value() ? *res : TObject};
}

std::optional<ProfileEntry> comparePayload(const CompareCache& cache) {
  const CompareCache::CompareSpecialization specialization =
      cache.specializedTypes();
  const std::optional<Type> left_type =
      specializedTypeToHirType(specialization.lhs);
  const std::optional<Type> right_type =
      specializedTypeToHirType(specialization.rhs);
  const std::optional<Type> res = specializedTypeToHirType(specialization.ret);
  if (!left_type.has_value() || !right_type.has_value()) {
    return std::nullopt;
  }
  return CompareProfile{
      *left_type, *right_type, res.has_value() ? *res : TObject};
}

std::optional<ProfileEntry> loadAttrPayload(const LoadAttrCache& cache) {
  const std::vector<LoadAttrCache::ProfiledEntry> entries =
      cache.profiledEntries();
  if (entries.empty()) {
    return std::nullopt;
  }
  if (entries.size() > 1) {
    return LoadAttrProfile{{}, std::nullopt, true};
  }
  const LoadAttrCache::ProfiledEntry& entry = entries.front();
  // Class-receiver entries store the class itself rather than
  // Py_TYPE(receiver); the kind tells the consumer to guard identity.
  return LoadAttrProfile{entry.type, entry.kind, false, entry.module};
}

// LoadMethod caches reuse LoadAttrProfile with no kind: only the receiver
// type (and the polymorphic marker) is meaningful for them.
std::optional<ProfileEntry> loadMethodPayload(const LoadMethodCache& cache) {
  const std::vector<LoadMethodCache::ProfiledEntry> entries =
      cache.profiledEntries();
  if (entries.empty()) {
    return std::nullopt;
  }
  if (entries.size() > 1) {
    return LoadAttrProfile{{}, std::nullopt, true};
  }
  const LoadMethodCache::ProfiledEntry& entry = entries.front();
  if (!entry.is_bound_method) {
    // Unbound entries (staticmethods, class variables, __getattr__ sentinels)
    // and class methods are not plain method calls on the receiver, so they
    // cannot be specialized as a receiver-type guard.
    return std::nullopt;
  }
  return LoadAttrProfile{entry.type, std::nullopt, false};
}

std::optional<ProfileEntry> storeAttrPayload(const StoreAttrCache& cache) {
  const std::vector<StoreAttrCache::ProfiledEntry> entries =
      cache.profiledEntries();
  if (entries.empty()) {
    return std::nullopt;
  }
  if (entries.size() > 1) {
    return StoreAttrProfile{{}, std::nullopt, true};
  }
  const StoreAttrCache::ProfiledEntry& entry = entries.front();
  return StoreAttrProfile{entry.type, entry.kind, false};
}

std::optional<ProfileEntry> cachePayload(const ProfileSource& source) {
  if (const auto* cache = std::get_if<BinaryOpCache*>(&source)) {
    return binaryOpPayload(**cache);
  }
  if (const auto* cache = std::get_if<CompareCache*>(&source)) {
    return comparePayload(**cache);
  }
  if (const auto* cache = std::get_if<LoadAttrCache*>(&source)) {
    return loadAttrPayload(**cache);
  }
  if (const auto* cache = std::get_if<LoadMethodCache*>(&source)) {
    return loadMethodPayload(**cache);
  }
  if (const auto* cache = std::get_if<StoreAttrCache*>(&source)) {
    return storeAttrPayload(**cache);
  }
  return std::nullopt;
}
// Merge the payloads observed at one location by several inline caches (see
// ProfileCollector::add). Returns nullopt when the observations
// contradict each other and their kind has no polymorphic representation; the
// caller then drops the entry entirely.
// Takes the observations by mutable reference and moves the surviving payload
// out; the vector is not used afterwards.
std::optional<ProfileEntry> mergePayloads(std::vector<ProfileEntry>& observed) {
  JIT_DCHECK(!observed.empty(), "merging requires at least one observation");
  const ProfileEntry& first = observed.front();
  if (std::all_of(
          observed.begin(), observed.end(), [&](const ProfileEntry& entry) {
            return entry == first;
          })) {
    return std::move(observed.front());
  }
  if (std::holds_alternative<LoadAttrProfile>(first)) {
    return LoadAttrProfile{{}, std::nullopt, true};
  }
  if (std::holds_alternative<StoreAttrProfile>(first)) {
    return StoreAttrProfile{{}, std::nullopt, true};
  }
  return std::nullopt;
}

// Take owning references for the borrowed pointers in a final entry, for
// Profile's owned set. Must be called with the GIL held.
void collectOwnedRefs(
    const ProfileEntry& entry,
    std::unordered_set<Ref<>>& owned) {
  collectProfileEntryRefs(entry, [&](BorrowedRef<> obj) {
    owned.insert(Ref<>::create(obj.get()));
  });
}

} // namespace

void ProfileCollector::add(ProfileLocation location, ProfileEntry payload) {
#ifndef ENABLE_PREFORK_MODEL
  secondary_entries_[std::move(location)].emplace_back(std::move(payload));
#endif
}

Profile ProfileCollector::snapshot() const {
#ifdef ENABLE_PREFORK_MODEL
  return Profile{};
#else
  std::unordered_map<ProfileLocation, ProfileEntry> entries;
  // Populate the uses first; live caches below overwrite them.
  for (const auto& [location, sources] : secondary_entries_) {
    std::vector<ProfileEntry> observed;
    observed.reserve(sources.size());
    for (const auto& source : sources) {
      observed.push_back(source);
    }
    if (auto merged = mergePayloads(observed)) {
      entries.insert_or_assign(location, std::move(merged.value()));
    }
    // Contradictory carried payloads with no polymorphic representation emit
    // nothing, so the next compile stays generic.
  }
  for (const auto& [location, sources] : entries_) {
    std::vector<ProfileEntry> observed;
    for (const auto& source : sources) {
      if (auto payload = cachePayload(source)) {
        observed.push_back(std::move(payload.value()));
      }
    }
    if (observed.empty()) {
      // Cold caches carry no new information; keep any carried payload.
      continue;
    }
    if (auto merged = mergePayloads(observed)) {
      entries.insert_or_assign(location, std::move(merged.value()));
    } else {
      // Contradictory observations with no polymorphic representation: drop
      // any carried payload so the next compile stays generic.
      entries.erase(location);
    }
  }
  std::unordered_set<Ref<>> owned_refs;
  for (const auto& [location, entry] : entries) {
    collectOwnedRefs(entry, owned_refs);
  }
  return Profile(std::move(entries), std::move(owned_refs));
#endif
}

} // namespace cinderx::jit
