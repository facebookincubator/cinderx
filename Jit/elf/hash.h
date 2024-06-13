// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/elf/string.h"
#include "cinderx/Jit/elf/symbol.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jit::elf {

// This is the hash function defined by the ELF standard.
constexpr uint32_t hash(const char* name) {
  uint32_t h = 0;
  for (; *name; name++) {
    h = (h << 4) + *name;
    uint32_t g = h & 0xf0000000;
    if (g) {
      h ^= g >> 24;
    }
    h &= ~g;
  }
  return h;
}

// Hash table of symbols.  The table is split into two arrays: the buckets array
// and the chains array.  The buckets array holds symbol table indices, and if
// those don't match, then the lookup starts chasing through the chains array,
// trying each index until it hits 0, which is always the undefined symbol.
//
// See
// https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-48031.html#scrolltoc
class HashTable {
 public:
  // Build a new hash table from a symbol and string table pair.
  void build(const SymbolTable& syms, const StringTable& strings);

  constexpr std::span<const uint32_t> buckets() const {
    return std::span{buckets_};
  }

  constexpr std::span<const uint32_t> chains() const {
    return std::span{chains_};
  }

  constexpr size_t size_bytes() const {
    // Hash table serializes the lengths of both tables as uint32_t values
    // before writing out the tables.
    return (sizeof(uint32_t) * 2) + buckets().size_bytes() +
        chains().size_bytes();
  }

 private:
  uint32_t chaseChainIdx(uint32_t idx) const;

  std::vector<uint32_t> buckets_;
  std::vector<uint32_t> chains_;
};

} // namespace jit::elf
