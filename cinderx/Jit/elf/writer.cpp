// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/writer.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/elf/string.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace cinderx::jit::elf {

namespace {

size_t alignedOffset(size_t offset, uint64_t alignment) {
  const uint64_t align = std::max<uint64_t>(alignment, 1);
  JIT_THROW_IF((align & (align - 1)) != 0, "Invalid ELF alignment {}", align);
  JIT_THROW_IF(
      align - 1 > std::numeric_limits<size_t>::max() - offset,
      "ELF offset overflow");
  return (offset + align - 1) & ~(align - 1);
}

void append(std::vector<std::byte>& output, std::span<const std::byte> bytes) {
  output.insert(output.end(), bytes.begin(), bytes.end());
}

} // namespace

std::vector<std::byte> writeSections(
    FileHeader header,
    std::span<const Section> sections) {
  JIT_THROW_IF(
      sections.size() >= 0xff00 - 2,
      "Too many ELF sections for a direct section count");
  JIT_THROW_IF(
      header.segment_header_count != 0, "ELF writer does not emit segments");

  std::vector<SectionHeader> headers(sections.size() + 2);
  StringTable names;
  header.segment_header_offset = 0;
  header.section_header_offset = sizeof(FileHeader);
  header.section_header_count = headers.size();
  header.section_name_index = headers.size() - 1;

  std::vector<std::byte> output(
      sizeof(FileHeader) + headers.size() * sizeof(SectionHeader));
  for (size_t i = 0; i < sections.size(); ++i) {
    const Section& section = sections[i];
    SectionHeader& sh = headers[i + 1];
    sh = section.header;
    sh.name_offset = names.insert(section.name);
    if (sh.type == kNoBits) {
      JIT_THROW_IF(!section.data.empty(), "NOBITS section has file data");
      sh.offset = 0;
      continue;
    }
    sh.offset = alignedOffset(output.size(), sh.align);
    sh.size = section.data.size();
    output.resize(sh.offset);
    append(output, section.data);
  }

  SectionHeader& shstrtab = headers.back();
  shstrtab.name_offset = names.insert(".shstrtab");
  shstrtab.type = kStringTable;
  shstrtab.align = 1;
  shstrtab.offset = output.size();
  shstrtab.size = names.bytes().size();
  append(output, names.bytes());

  std::memcpy(output.data(), &header, sizeof(header));
  std::memcpy(
      output.data() + header.section_header_offset,
      headers.data(),
      headers.size() * sizeof(SectionHeader));
  return output;
}

} // namespace cinderx::jit::elf
