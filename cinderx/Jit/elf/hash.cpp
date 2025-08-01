// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/hash.h"

#include "cinderx/Common/log.h"

namespace jit::elf {

void HashTable::build(const SymbolTable& syms, const StringTable& strings) {
  // Use a load factor of 2 for the hash table.  It will never be resized
  // after it is created.
  buckets_.reserve(syms.size() / 2);
  buckets_.resize(syms.size() / 2);

  chains_.reserve(syms.size());
  chains_.resize(syms.size());

  // Skip element zero as that's the undefined symbol.
  for (size_t i = 1; i < syms.size(); ++i) {
    auto const bucket_idx =
        hash(strings.string_at(syms[i].name_offset).data()) % buckets_.size();
    auto first_chain_idx = buckets_[bucket_idx];
    if (first_chain_idx == 0) {
      buckets_[bucket_idx] = i;
    } else {
      chains_[chaseChainIdx(first_chain_idx)] = i;
    }
  }
}

uint32_t HashTable::chaseChainIdx(uint32_t idx) const {
  const uint32_t limit = chains_.size();

  uint32_t count;
  for (count = 0; chains_[idx] != 0 && count < limit; ++count) {
    idx = chains_[idx];
  }
  JIT_CHECK(
      count < limit,
      "Can't find end of hash table chain, infinite loop, last index {}",
      idx);

  return idx;
}

} // namespace jit::elf
