// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/define.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/symbol.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/gdb_support.h"
#include "cinderx/Jit/jit_unwind.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace cinderx {

using namespace cinderx::jit;
using ElfTest = RuntimeTest;

namespace {

template <class T>
T readValue(std::span<const std::byte> bytes, size_t offset = 0) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error{"Truncated test ELF value"};
  }
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

std::optional<elf::SectionHeader> findHeader(
    std::span<const std::byte> bytes,
    std::string_view name) {
  const elf::FileHeader header = readValue<elf::FileHeader>(bytes);
  const std::span<const std::byte> names = elf::findSection(bytes, ".shstrtab");
  for (size_t i = 0; i < header.section_header_count; ++i) {
    const elf::SectionHeader section = readValue<elf::SectionHeader>(
        bytes, header.section_header_offset + i * sizeof(elf::SectionHeader));
    if (section.name_offset >= names.size()) {
      throw std::runtime_error{"Invalid test ELF name"};
    }
    const char* section_name =
        reinterpret_cast<const char*>(names.data() + section.name_offset);
    if (name == section_name) {
      return section;
    }
  }
  return std::nullopt;
}

std::vector<std::byte> debugObject(
    std::string_view function_name = "test_function",
    std::string_view filename = "spaghetti.py") {
  return buildDebugElf(
      function_name, filename, 15, reinterpret_cast<void*>(0x10000), 4);
}

} // namespace

TEST(ElfWriterTest, DebugObjectSymbolsAndHeaders) {
  const std::vector<std::byte> bytes = debugObject();
  const elf::FileHeader header = readValue<elf::FileHeader>(bytes);
  constexpr uint8_t magic[]{0x7f, 'E', 'L', 'F'};
  EXPECT_EQ(std::memcmp(header.magic, magic, sizeof(magic)), 0);
  EXPECT_EQ(header.type, 1); // ET_REL, not a loadable AOT library.
  EXPECT_EQ(header.osabi, 0);
  EXPECT_EQ(header.elf_class, 2);
  EXPECT_EQ(header.endian, 1);
  EXPECT_EQ(header.machine, kBuildArch == Arch::kAarch64 ? 183 : 62);
  EXPECT_EQ(header.segment_header_count, 0);
  EXPECT_EQ(header.segment_header_size, 0);

  const std::optional<elf::SectionHeader> text = findHeader(bytes, ".text");
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text->type, 8); // SHT_NOBITS.
  EXPECT_EQ(text->flags, 6); // SHF_ALLOC | SHF_EXECINSTR.
  EXPECT_EQ(text->address, 0x10000);
  EXPECT_EQ(text->size, 4);
  EXPECT_TRUE(elf::findSection(bytes, ".text").empty());

  const std::optional<elf::SectionHeader> symtab_header =
      findHeader(bytes, ".symtab");
  const std::optional<elf::SectionHeader> strtab_header =
      findHeader(bytes, ".strtab");
  ASSERT_TRUE(symtab_header.has_value());
  ASSERT_TRUE(strtab_header.has_value());
  EXPECT_EQ(symtab_header->info, 2);
  EXPECT_EQ(symtab_header->entry_size, 24);
  EXPECT_EQ(symtab_header->offset % symtab_header->align, 0);
  const elf::SectionHeader linked = readValue<elf::SectionHeader>(
      bytes,
      header.section_header_offset +
          symtab_header->link * sizeof(elf::SectionHeader));
  EXPECT_EQ(linked.offset, strtab_header->offset);

  const std::span<const std::byte> symbols = elf::findSection(bytes, ".symtab");
  ASSERT_EQ(symbols.size(), 3 * sizeof(elf::Symbol));
  const elf::Symbol file = readValue<elf::Symbol>(symbols, sizeof(elf::Symbol));
  const elf::Symbol func =
      readValue<elf::Symbol>(symbols, 2 * sizeof(elf::Symbol));
  const std::span<const std::byte> strings = elf::findSection(bytes, ".strtab");
  ASSERT_LT(file.name_offset, strings.size());
  ASSERT_LT(func.name_offset, strings.size());
  EXPECT_STREQ(
      reinterpret_cast<const char*>(strings.data() + file.name_offset),
      "cinderjit");
  EXPECT_STREQ(
      reinterpret_cast<const char*>(strings.data() + func.name_offset),
      "test_function");
  EXPECT_EQ(file.info, 4); // Local STT_FILE.
  EXPECT_EQ(file.section_index, 0xfff1); // SHN_ABS.
  EXPECT_EQ(func.info, 0x12); // Global STT_FUNC.
  EXPECT_EQ(func.section_index, 1);
  EXPECT_EQ(func.address, 0); // Relative to .text.
  EXPECT_EQ(func.size, 4);
}

TEST(ElfWriterTest, DebugObjectDwarfAndUnwind) {
  const std::vector<std::byte> bytes = debugObject();
  const std::span<const std::byte> info =
      elf::findSection(bytes, ".debug_info");
  EXPECT_EQ(readValue<uint32_t>(info), info.size() - 4);
  EXPECT_EQ(readValue<uint16_t>(info, 4), 2);
  ASSERT_GT(info.size(), 12 + std::strlen("spaghetti.py"));
  EXPECT_STREQ(reinterpret_cast<const char*>(info.data() + 12), "spaghetti.py");
  const size_t low_pc = 12 + std::strlen("spaghetti.py") + 1;
  EXPECT_EQ(readValue<uintptr_t>(info, low_pc), 0x10000);
  EXPECT_EQ(readValue<uintptr_t>(info, low_pc + sizeof(uintptr_t)), 0x10004);

  const std::span<const std::byte> line =
      elf::findSection(bytes, ".debug_line");
  const uint32_t header_size = readValue<uint32_t>(line, 6);
  const size_t program = 10 + header_size;
  ASSERT_LE(program + 3 + sizeof(uintptr_t) + 8, line.size());
  EXPECT_EQ(readValue<uint8_t>(line, program + 2), 2); // DW_LNE_set_address.
  EXPECT_EQ(readValue<uintptr_t>(line, program + 3), 0x10000);
  constexpr uint8_t expected_tail[]{3, 14, 1, 2, 4, 0, 1, 1};
  EXPECT_EQ(
      std::memcmp(
          line.data() + program + 3 + sizeof(uintptr_t),
          expected_tail,
          sizeof(expected_tail)),
      0);

  std::vector<uint8_t> expected_frame(jitUnwindEhFrameSize());
  const void* code = reinterpret_cast<void*>(0x10000);
  expected_frame.resize(buildJitUnwindEhFrame(
      expected_frame.data(), expected_frame.size(), code, 4));
  const std::span<const std::byte> frame = elf::findSection(bytes, ".eh_frame");
  ASSERT_EQ(frame.size(), expected_frame.size());
  EXPECT_EQ(std::memcmp(frame.data(), expected_frame.data(), frame.size()), 0);
  const std::optional<elf::SectionHeader> frame_header =
      findHeader(bytes, ".eh_frame");
  ASSERT_TRUE(frame_header.has_value());
  EXPECT_EQ(frame_header->address, 0x10008);
  EXPECT_EQ(frame_header->offset % frame_header->align, 0);
}

TEST(ElfWriterTest, LongNamesAreNotLimitedToFixedScratch) {
  const std::string function_name(10000, 'f');
  const std::string filename(12000, 'p');
  const std::vector<std::byte> bytes = debugObject(function_name, filename);
  const std::span<const std::byte> strings = elf::findSection(bytes, ".strtab");
  const std::span<const std::byte> symbols = elf::findSection(bytes, ".symtab");
  const elf::Symbol func =
      readValue<elf::Symbol>(symbols, 2 * sizeof(elf::Symbol));
  ASSERT_LT(func.name_offset, strings.size());
  EXPECT_EQ(
      std::string_view{
          reinterpret_cast<const char*>(strings.data() + func.name_offset)},
      function_name);
  const std::span<const std::byte> info =
      elf::findSection(bytes, ".debug_info");
  EXPECT_EQ(
      std::string_view{reinterpret_cast<const char*>(info.data() + 12)},
      filename);
}

TEST(ElfWriterTest, UnknownNamesAndMultibyteLineProgram) {
  const std::vector<std::byte> bytes =
      buildDebugElf("", "", 200, reinterpret_cast<void*>(0x10000), 1024);
  const std::span<const std::byte> info =
      elf::findSection(bytes, ".debug_info");
  EXPECT_STREQ(reinterpret_cast<const char*>(info.data() + 12), "<unknown>");
  const std::span<const std::byte> line =
      elf::findSection(bytes, ".debug_line");
  const size_t program = 10 + readValue<uint32_t>(line, 6);
  constexpr uint8_t expected_tail[]{3, 0xc7, 1, 1, 2, 0x80, 8, 0, 1, 1};
  ASSERT_LE(
      program + 3 + sizeof(uintptr_t) + sizeof(expected_tail), line.size());
  EXPECT_EQ(
      std::memcmp(
          line.data() + program + 3 + sizeof(uintptr_t),
          expected_tail,
          sizeof(expected_tail)),
      0);
}

TEST(ElfWriterTest, RejectsUnrepresentableCodeRanges) {
  EXPECT_THROW(
      buildDebugElf(
          "f",
          "f.py",
          1,
          reinterpret_cast<void*>(0x10000),
          size_t{UINT32_MAX} + 1),
      std::runtime_error);
  EXPECT_THROW(
      buildDebugElf(
          "f", "f.py", 1, reinterpret_cast<void*>(UINTPTR_MAX - 3), 4),
      std::runtime_error);
}

TEST(ElfWriterTest, EmptySectionsAndAlignment) {
  const std::vector<std::byte> empty = elf::writeSections({}, {});
  EXPECT_FALSE(elf::findSection(empty, ".shstrtab").empty());
  EXPECT_TRUE(elf::findSection(empty, ".missing").empty());
  constexpr uint8_t payload[]{1, 2, 3};
  const std::array<elf::Section, 2> sections{{
      {".data",
       {.type = elf::kProgram, .align = 16},
       std::as_bytes(std::span{payload})},
      {".empty", {.type = elf::kProgram, .align = 64}, {}},
  }};
  const std::vector<std::byte> bytes = elf::writeSections({}, sections);
  const std::span<const std::byte> data = elf::findSection(bytes, ".data");
  ASSERT_EQ(data.size(), sizeof(payload));
  EXPECT_EQ(std::memcmp(data.data(), payload, sizeof(payload)), 0);
  const std::optional<elf::SectionHeader> data_header =
      findHeader(bytes, ".data");
  const std::optional<elf::SectionHeader> empty_header =
      findHeader(bytes, ".empty");
  ASSERT_TRUE(data_header.has_value());
  ASSERT_TRUE(empty_header.has_value());
  EXPECT_EQ(data_header->offset % 16, 0);
  EXPECT_EQ(empty_header->offset % 64, 0);
  EXPECT_TRUE(elf::findSection(bytes, ".empty").empty());
}

TEST(ElfReaderTest, RejectsTruncatedOrOutOfBoundsHeaders) {
  EXPECT_THROW(elf::findSection({}, ".text"), std::runtime_error);
  std::vector<std::byte> bytes = debugObject();
  EXPECT_THROW(
      elf::findSection(std::span{bytes}.first(63), ".text"),
      std::runtime_error);
  elf::FileHeader header = readValue<elf::FileHeader>(bytes);
  header.section_header_offset = UINT64_MAX;
  std::memcpy(bytes.data(), &header, sizeof(header));
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
}

TEST(ElfReaderTest, RejectsInvalidHeadersAndUnterminatedNames) {
  std::vector<std::byte> bytes = debugObject();
  elf::FileHeader header = readValue<elf::FileHeader>(bytes);
  header.magic[0] = 0;
  std::memcpy(bytes.data(), &header, sizeof(header));
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
  header.magic[0] = 0x7f;
  header.section_name_index = header.section_header_count;
  std::memcpy(bytes.data(), &header, sizeof(header));
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);

  bytes = debugObject();
  const std::optional<elf::SectionHeader> names =
      findHeader(bytes, ".shstrtab");
  ASSERT_TRUE(names.has_value());
  std::memset(bytes.data() + names->offset, 'x', names->size);
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
}

TEST(ElfReaderTest, RejectsOutOfBoundsSectionNamesAndData) {
  std::vector<std::byte> bytes = debugObject();
  const elf::FileHeader header = readValue<elf::FileHeader>(bytes);
  const size_t section_offset =
      header.section_header_offset + sizeof(elf::SectionHeader);
  elf::SectionHeader section =
      readValue<elf::SectionHeader>(bytes, section_offset);
  section.name_offset = UINT32_MAX;
  std::memcpy(bytes.data() + section_offset, &section, sizeof(section));
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
  section.name_offset = 1;
  section.type = elf::kProgram;
  section.offset = UINT64_MAX;
  std::memcpy(bytes.data() + section_offset, &section, sizeof(section));
  EXPECT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
}

} // namespace cinderx
