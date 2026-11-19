// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace cinderx::jit::elf {

constexpr uint32_t kProgram = 0x01;
constexpr uint32_t kSymbolTable = 0x02;
constexpr uint32_t kStringTable = 0x03;
constexpr uint32_t kNoBits = 0x08;

constexpr uint64_t kSectionWritable = 0x01;
constexpr uint64_t kSectionAlloc = 0x02;
constexpr uint64_t kSectionExecutable = 0x04;

// See https://en.wikipedia.org/wiki/Executable_and_Linkable_Format#File_layout
// for the ELF64 layouts.
struct SectionHeader {
  // Offset of the section name in .shstrtab.
  uint32_t name_offset{0};

  // Section type, such as program data or a symbol table.
  uint32_t type{0};

  // Section flags, such as writable, allocated, or executable.
  uint64_t flags{0};

  // Virtual address of the section when loaded into memory.
  uint64_t address{0};

  // Offset of the section in the file.
  uint64_t offset{0};

  // Size in bytes, including memory-only NOBITS sections.
  uint64_t size{0};

  // Associated section index; .symtab links to .strtab.
  uint32_t link{0};

  // Section-specific data; .symtab uses the first global symbol index.
  uint32_t info{0};

  // Required alignment: zero for none, otherwise a power of two.
  uint64_t align{0};

  // Size of a fixed-size entry, or zero for other section types.
  uint64_t entry_size{0};
};

// Defaults describe a little-endian relocatable object without program headers.
struct FileHeader {
  // ELF magic bytes.
  uint8_t magic[4]{0x7f, 'E', 'L', 'F'};

  // 64-bit.
  uint8_t elf_class{2};

  // Little endian.
  uint8_t endian{1};

  // ELF version is always 1.
  uint8_t elf_version{1};

  // System V ABI, as expected by GDB.
  uint8_t osabi{0};

  // ABI version, unused for System V.
  uint8_t abi_version{0};

  // Reserved bytes.
  uint8_t padding[7] = {0};

  // Relocatable object (ET_REL).
  uint16_t type{1};

  // Target architecture; defaults to AMD x86-64.
  uint16_t machine{0x3e};

  // Duplicate of the previous version field.
  uint32_t version{1};

  // Entry point for executable files, unused for debug objects.
  uint64_t entry_address{0};

  // File offset of the program header table.
  uint64_t segment_header_offset{0};

  // File offset of the section header table.
  uint64_t section_header_offset{0};

  // Architecture-specific flags.
  uint32_t flags{0};

  // Size of this ELF64 file header.
  uint16_t header_size{64};

  // Size of a program header; zero when none are emitted.
  uint16_t segment_header_size{0};

  // Number of program headers.
  uint16_t segment_header_count{0};

  // Size of one section header.
  uint16_t section_header_size{sizeof(SectionHeader)};

  // Number of section headers, including the null section.
  uint16_t section_header_count{0};

  // Index of the section containing the section-name string table.
  uint16_t section_name_index{0};
};

} // namespace cinderx::jit::elf
