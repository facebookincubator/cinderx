// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jit::elf {

enum class DynTag : uint64_t {
  kNull = 0,
  kNeeded = 1,
  kHash = 4,
  kStrtab = 5,
  kSymtab = 6,
  kStrSz = 10,
  kSymEnt = 11,
};

struct Dyn {
  constexpr Dyn() = default;
  constexpr Dyn(DynTag tag, uint64_t val) : tag{tag}, val{val} {}

  DynTag tag{DynTag::kNull};
  uint64_t val{0};
};

class DynamicTable {
 public:
  DynamicTable();

  template <class... Args>
  void insert(Args&&... args) {
    dyns_.emplace_back(std::forward<Args>(args)...);
    // Always swap the null item back to the end.
    auto const len = dyns_.size();
    JIT_DCHECK(len >= 2, "DynamicTable missing its required null item");
    std::swap(dyns_[len - 1], dyns_[len - 2]);
  }

  std::span<const std::byte> bytes() const {
    return std::as_bytes(std::span{dyns_});
  }

 private:
  std::vector<Dyn> dyns_;
};

} // namespace jit::elf
