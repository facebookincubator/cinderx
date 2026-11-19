// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/reader.h"

#include "cinderx/Jit/elf/header.h"

#include <cstring>
#include <stdexcept>

namespace cinderx::jit::elf {

namespace {

std::span<const std::byte>
slice(std::span<const std::byte> bytes, uint64_t offset, uint64_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset) {
    throw std::runtime_error{"ELF section extends past the object"};
  }
  return bytes.subspan(offset, size);
}

template <class T>
T read(std::span<const std::byte> bytes, uint64_t offset) {
  T value;
  std::memcpy(&value, slice(bytes, offset, sizeof(T)).data(), sizeof(T));
  return value;
}

} // namespace

std::span<const std::byte> findSection(
    std::span<const std::byte> elf,
    std::string_view name) {
  const FileHeader header = read<FileHeader>(elf, 0);
  constexpr uint8_t magic[]{0x7f, 'E', 'L', 'F'};
  if (std::memcmp(header.magic, magic, sizeof(magic)) != 0 ||
      header.elf_class != 2 || header.endian != 1 || header.elf_version != 1 ||
      header.version != 1 ||
      header.section_header_size != sizeof(SectionHeader) ||
      header.section_name_index >= header.section_header_count) {
    throw std::runtime_error{"Invalid ELF64 header"};
  }
  const std::span<const std::byte> sections = slice(
      elf,
      header.section_header_offset,
      uint64_t{header.section_header_count} * sizeof(SectionHeader));
  const SectionHeader names_header = read<SectionHeader>(
      sections, header.section_name_index * sizeof(SectionHeader));
  const std::span<const std::byte> names =
      slice(elf, names_header.offset, names_header.size);

  for (size_t i = 0; i < header.section_header_count; ++i) {
    const SectionHeader section =
        read<SectionHeader>(sections, i * sizeof(SectionHeader));
    if (section.name_offset >= names.size()) {
      throw std::runtime_error{"ELF section name is outside .shstrtab"};
    }
    const char* start =
        reinterpret_cast<const char*>(names.data() + section.name_offset);
    const char* end = static_cast<const char*>(
        std::memchr(start, 0, names.size() - section.name_offset));
    if (end == nullptr) {
      throw std::runtime_error{"Unterminated ELF section name"};
    }
    if (std::string_view{start, static_cast<size_t>(end - start)} == name) {
      return section.type == kNoBits ? std::span<const std::byte>{}
                                     : slice(elf, section.offset, section.size);
    }
  }
  return {};
}

} // namespace cinderx::jit::elf
