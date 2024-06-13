// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/dynamic.h"

#include <type_traits>

namespace jit::elf {

static_assert(std::is_standard_layout_v<Dyn>);

DynamicTable::DynamicTable() {
  // Table must always end with a null dynamic item.
  dyns_.emplace_back();
}

} // namespace jit::elf
