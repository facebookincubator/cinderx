// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace jit::elf {

// String table encoded for ELF.
class StringTable {
 public:
  StringTable();

  // Insert a string into the symbol table, return its offset.
  uint32_t insert(std::string_view s);

  // Get the string at a given offset.
  std::string_view string_at(size_t offset) const;

  std::span<const std::byte> bytes() const {
    return std::as_bytes(std::span<const uint8_t>{bytes_});
  }

 private:
  std::vector<uint8_t> bytes_;
};

} // namespace jit::elf
