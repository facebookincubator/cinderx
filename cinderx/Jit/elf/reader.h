// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace cinderx::jit::elf {

// Find file-backed data in a little-endian ELF64 object. Malformed objects
// throw std::runtime_error. Missing and NOBITS sections return an empty span.
std::span<const std::byte> findSection(
    std::span<const std::byte> elf,
    std::string_view name);

} // namespace cinderx::jit::elf
