// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/elf/header.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace cinderx::jit::elf {

struct Section {
  // Section name to insert into the generated .shstrtab.
  std::string_view name;

  // Section attributes; the writer fills in file offsets and sizes.
  SectionHeader header;

  // File-backed contents, empty for NOBITS sections.
  std::span<const std::byte> data;
};

// Serialize an ELF64 object with a null section followed by `sections` and a
// generated .shstrtab. Section indices in links/symbols are therefore
// one-based. NOBITS sections describe memory without contributing bytes to the
// object.
std::vector<std::byte> writeSections(
    FileHeader header,
    std::span<const Section> sections);

} // namespace cinderx::jit::elf
