// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/string.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <cstring>

namespace jit::elf {

StringTable::StringTable() {
  // All string tables begin with a NUL character.
  bytes_.push_back(0);
}

uint32_t StringTable::insert(std::string_view s) {
  size_t start_off = bytes_.size();
  // Strings are always encoded with a NUL terminator.
  bytes_.resize(bytes_.size() + s.size() + 1);
  std::memcpy(&bytes_[start_off], s.data(), s.size());
  JIT_CHECK(
      fitsSignedInt<32>(start_off),
      "ELF symbol table only deals in 32-bit offsets");
  return static_cast<uint32_t>(start_off);
}

std::string_view StringTable::string_at(size_t offset) const {
  return reinterpret_cast<const char*>(&bytes_[offset]);
}

} // namespace jit::elf
