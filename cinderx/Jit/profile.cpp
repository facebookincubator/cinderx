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

std::optional<ProfileEntry> cachePayload(const ProfileSource& source) {
  if (const auto* cache = std::get_if<BinaryOpCache*>(&source)) {
    return binaryOpPayload(**cache);
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
  return std::nullopt;
}

} // namespace

void ProfileCollector::add(
    BorrowedRef<PyCodeObject> code,
    BCOffset offset,
    BinaryOpCache* cache) {
#ifndef ENABLE_PREFORK_MODEL
  entries_[ProfileLocation(code, offset)].emplace_back(cache);
#endif
}

Profile ProfileCollector::snapshot() const {
#ifdef ENABLE_PREFORK_MODEL
  return Profile{};
#else
  std::unordered_map<ProfileLocation, ProfileEntry> entries;
  // Populate the uses first; live caches below overwrite them.
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

  return Profile(std::move(entries));
#endif
}

} // namespace cinderx::jit
