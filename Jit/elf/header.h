// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace jit::elf {

// See https://en.wikipedia.org/wiki/Executable_and_Linkable_Format#File_layout
// for how the ELF 64-bit file layout is structured.

// Section header types.
constexpr uint32_t kProgram = 0x01;
constexpr uint32_t kSymbolTable = 0x02;
constexpr uint32_t kStringTable = 0x03;
constexpr uint32_t kHash = 0x05;
constexpr uint32_t kDynamic = 0x06;
constexpr uint32_t kNote = 0x07;

// Section header flags.
constexpr uint64_t kSectionWritable = 0x01;
constexpr uint64_t kSectionAlloc = 0x02;
constexpr uint64_t kSectionExecutable = 0x04;
constexpr uint64_t kSectionInfoLink = 0x40;

// Segment header types.
constexpr uint32_t kSegmentLoadable = 0x1;
constexpr uint32_t kSegmentDynamic = 0x2;
constexpr uint32_t kSegmentNote = 0x4;

// Segment header flags.
constexpr uint32_t kSegmentExecutable = 0x1;
constexpr uint32_t kSegmentWritable = 0x2;
constexpr uint32_t kSegmentReadable = 0x4;

// Header that describes an ELF section.
struct SectionHeader {
  // Offset into .shstrtab section for the name of this section.
  uint32_t name_offset{0};

  // Type of this section (e.g. program data, symbol table, etc.).
  uint32_t type{0};

  // Flags for the section (e.g. writable, executable, contains strings, etc.).
  uint64_t flags{0};

  // Virtual address of the section in memory, if it is loaded into memory.
  uint64_t address{0};

  // Offset of the section in the file.
  uint64_t offset{0};

  // Size of the section in the file.
  uint64_t size{0};

  // Use depends on the section type.  Generally only needed for the symbol
  // table for Cinder's purposes.
  uint32_t link{0};
  uint32_t info{0};

  // Required alignment of the section.  An alignment of 0 means the section is
  // unaligned, otherwise the value must be a power of two.
  uint64_t align{0};

  // If this is a special section type that contains fixed-size entries
  // (e.g. symbol table), then this will be the entry size.  Zero for all other
  // sections.
  uint64_t entry_size{0};
};

// Header that describes a memory segment loaded from an ELF file.
struct SegmentHeader {
  // Type of this segment.
  uint32_t type{0};

  // Executable, writable, and readable bits.
  uint32_t flags{0};

  // Offset of the memory segment in the file.
  uint64_t offset{0};

  // Virtual address of the memory segment.
  uint64_t address{0};

  // Unused, only applies to systems where raw physical memory access is
  // relevant.
  const uint64_t physical_address{0};

  // Size of the memory segment.
  //
  // `file_size` is the number of bytes inside of the ELF file, but `mem_size`
  // is how big the segment should be after it is loaded.  If `mem_size` is
  // bigger than `file_size`, then the remaining bytes will be padded with zeros
  // when it is loaded.  This is how .bss gets implemented.
  uint64_t file_size{0};
  uint64_t mem_size{0};

  // Required alignment of the memory segment.  An alignment of 0 or 1 means the
  // segment is unaligned, otherwise the value must be a power of two.
  uint64_t align{0};
};

// Header of an ELF file.  Comes with some default values for convenience.
struct FileHeader {
  const uint8_t magic[4]{0x7f, 'E', 'L', 'F'};

  // 64-bit.
  uint8_t elf_class{2};

  // Little endian.
  uint8_t endian{1};

  // ELF version is always 1.
  const uint8_t elf_version{1};

  // Linux.
  uint8_t osabi{3};

  // Unused on Linux.
  uint8_t abi_version{0};
  uint8_t padding[7] = {0};

  // Dynamic library.
  uint16_t type{3};

  // AMD x86-64.
  uint16_t machine{0x3e};

  // Duplicate of the previous version field.
  const uint32_t version{1};

  // For executable files, this is where the program starts.
  const uint64_t entry_address{0};

  // Will point to the start of the segment and section header tables.
  uint64_t segment_header_offset{0};
  uint64_t section_header_offset{0};

  // Unused for x86, very likely unused for x86-64 as well.
  const uint32_t flags{0};

  // The size of this struct itself.
  const uint16_t header_size{64};

  // Size and number of segment headers.
  const uint16_t segment_header_size{sizeof(SegmentHeader)};
  uint16_t segment_header_count{0};

  // Size and number of section headers.
  const uint16_t section_header_size{sizeof(SectionHeader)};
  uint16_t section_header_count{0};

  // Section header table index that contains the section names table.
  uint16_t section_name_index{0};
};

} // namespace jit::elf
