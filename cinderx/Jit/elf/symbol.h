// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace cinderx::jit::elf {

// Symbol flags.
constexpr uint8_t kGlobal = 0x10;
constexpr uint8_t kFunc = 0x02;
constexpr uint8_t kFile = 0x04;
constexpr uint16_t kAbsoluteSection = 0xfff1;

struct Symbol {
  // Offset of the symbol name in the linked string table.
  uint32_t name_offset{0};

  // Symbol binding and type, packed into one byte.
  uint8_t info{0};

  // Symbol visibility; zero means default visibility.
  uint8_t other{0};

  // Index of the section containing this symbol.
  uint16_t section_index{0};

  // Symbol value; section-relative for relocatable objects.
  uint64_t address{0};

  // Size of the symbol in bytes.
  uint64_t size{0};
};

class SymbolTable {
 public:
  SymbolTable();

  template <class... Args>
  void insert(Args&&... args) {
    syms_.emplace_back(std::forward<Args>(args)...);
  }

  std::span<const std::byte> bytes() const {
    return std::as_bytes(std::span{syms_});
  }

 private:
  std::vector<Symbol> syms_;
};

} // namespace cinderx::jit::elf
