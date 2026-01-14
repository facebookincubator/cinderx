// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace jit::elf {

// Symbol flags.
constexpr uint8_t kGlobal = 0x10;
constexpr uint8_t kFunc = 0x02;

struct Symbol {
  uint32_t name_offset{0};

  // Type of symbol this is.
  uint8_t info{0};

  // Controls symbol visibility.  Zero means to compute visibility from the
  // `info` field.
  const uint8_t other{0};

  // Index of the section that this symbol points to.
  uint16_t section_index{0};
  uint64_t address{0};
  uint64_t size{0};
};

class SymbolTable {
 public:
  SymbolTable();

  template <class... Args>
  void insert(Args&&... args) {
    syms_.emplace_back(std::forward<Args>(args)...);
  }

  const Symbol& operator[](size_t idx) const;

  std::span<const std::byte> bytes() const {
    return std::as_bytes(std::span{syms_});
  }

  // Get the number of symbols in the table.
  constexpr size_t size() const {
    return syms_.size();
  }

 private:
  std::vector<Symbol> syms_;
};

} // namespace jit::elf
