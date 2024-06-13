// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/symbol.h"

#include <type_traits>

namespace jit::elf {

static_assert(std::is_standard_layout_v<Symbol>);

SymbolTable::SymbolTable() {
  // Symbol table must always start with an undefined symbol.
  insert(Symbol{});
}

const Symbol& SymbolTable::operator[](size_t idx) const {
  return syms_[idx];
}

} // namespace jit::elf
