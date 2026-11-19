// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/symbol.h"

#include <type_traits>

namespace cinderx::jit::elf {

static_assert(std::is_standard_layout_v<Symbol>);
static_assert(std::is_trivially_copyable_v<Symbol>);
static_assert(sizeof(Symbol) == 24);

SymbolTable::SymbolTable() {
  // Symbol table must always start with an undefined symbol.
  insert(Symbol{});
}

} // namespace cinderx::jit::elf
