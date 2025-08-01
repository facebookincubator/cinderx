// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/elf/dynamic.h"
#include "cinderx/Jit/elf/hash.h"
#include "cinderx/Jit/elf/header.h"
#include "cinderx/Jit/elf/note.h"
#include "cinderx/Jit/elf/string.h"
#include "cinderx/Jit/elf/symbol.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace jit::elf {

// Section header indices / ordering.
enum class SectionIdx : uint32_t {
  // Null section is index 0.

  kText = 1,
  kDynsym,
  kDynstr,
  kHash,
  kFuncNote,
  kDynamic,
  kShstrtab,
  kTotal,
};

// Segment header indices / ordering.
enum class SegmentIdx : uint32_t {
  kText,
  kReadonly,
  kReadwrite,
  kFuncNote,
  kDynamic,
  kTotal,
};

constexpr uint32_t raw(SectionIdx idx) {
  return static_cast<uint32_t>(idx);
}

constexpr uint32_t raw(SegmentIdx idx) {
  return static_cast<uint32_t>(idx);
}

// Represents an ELF object/file.
struct Object {
  FileHeader file_header;
  std::array<SectionHeader, raw(SectionIdx::kTotal)> section_headers;
  std::array<SegmentHeader, raw(SegmentIdx::kTotal)> segment_headers;

  // Amount of padding to put after the headers.  When used with offsetof, tells
  // us the total size of the headers.
  uint32_t header_padding{0};

  // This is the padding for the text section, which doesn't show up in this
  // struct.  It's the vector of CodeEntry objects passed to writeEntries().
  uint32_t text_padding{0};

  SymbolTable dynsym;
  StringTable dynstr;
  uint32_t dynsym_padding{0};

  HashTable hash;
  uint32_t hash_padding{0};

  NoteArray func_notes;
  uint32_t func_notes_padding{0};

  DynamicTable dynamic;
  uint32_t dynamic_padding{0};

  StringTable shstrtab;

  uint32_t section_offset{0};
  uint32_t libpython_name{0};

  SectionHeader& getSectionHeader(SectionIdx idx) {
    return section_headers[raw(idx)];
  }

  SegmentHeader& getSegmentHeader(SegmentIdx idx) {
    return segment_headers[raw(idx)];
  }
};

// Code entry to add to an ELF file.
struct CodeEntry {
  BorrowedRef<PyCodeObject> code;
  std::span<const std::byte> compiled_code;
  void* normal_entry{nullptr};
  void* static_entry{nullptr};
  std::string func_name;
  std::string file_name;
  size_t lineno{0};
};

// Write function or code objects out to a new ELF file.
//
// The output ELF file is always a shared library.
void writeEntries(std::ostream& os, const std::vector<CodeEntry>& entries);

} // namespace jit::elf
