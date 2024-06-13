// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/header.h"

#include <type_traits>

namespace jit::elf {

// ELF headers are all expected to be a set size.
static_assert(sizeof(SectionHeader) == 64);
static_assert(sizeof(SegmentHeader) == 56);
static_assert(sizeof(FileHeader) == FileHeader{}.header_size);

// Headers should be simple and in standard layout.  They're not trivial though
// because they zero-initialize themselves.
static_assert(std::is_standard_layout_v<SectionHeader>);
static_assert(std::is_standard_layout_v<SegmentHeader>);
static_assert(std::is_standard_layout_v<FileHeader>);

} // namespace jit::elf
