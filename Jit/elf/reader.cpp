// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/reader.h"

#ifdef ENABLE_ELF_READER
#include <elf.h>
#include <link.h>
#endif

#include <istream>
#include <sstream>

namespace jit::elf {

namespace {

template <class T, class U>
static bool contains(std::span<T> bigger, std::span<U> smaller) {
  return reinterpret_cast<T*>(smaller.data()) >= bigger.data() &&
      reinterpret_cast<T*>(&*smaller.end()) <= &*bigger.end();
}

// Given the previous item's padding alignment, skip over any padding that was
// added.
void unpad(std::istream& is, size_t padding) {
  size_t previous_size = is.gcount();

  // e.g. Wrote 23 with padding 4, means 1 byte to ignore.
  size_t ignore = padding - (previous_size % padding);
  if (ignore == padding) {
    ignore = 0;
  }

  is.ignore(ignore);
}

template <class T>
T readInt(std::istream& is) {
  T result = 0;
  is.read(reinterpret_cast<char*>(&result), sizeof(T));
  if (is.gcount() != sizeof(T)) {
    throw std::runtime_error{
        fmt::format("Failed to read int of size {}", sizeof(T))};
  }
  return result;
}

template <bool hasNulTerminator = true>
std::string readStr(std::istream& is, size_t size) {
  if (size > 100000) {
    throw std::runtime_error{fmt::format(
        "Trying to read string of size {}, something is likely wrong", size)};
  }

  std::string result;
  result.resize(size);
  // Strings are encoded with the NUL terminator so it needs to be read out
  // too.
  size_t read_size = hasNulTerminator ? size + 1 : size;

  is.read(result.data(), read_size);
  if (is.gcount() != read_size) {
    throw std::runtime_error{
        fmt::format("Failed to read str of size {}", read_size)};
  }
  return result;
}

Note readNote(std::istream& is) {
  // Encoded string sizes include the NUL terminators.
  auto name_size = readInt<uint32_t>(is) - 1;
  auto desc_size = readInt<uint32_t>(is) - 1;
  auto note_type = readInt<uint32_t>(is);

  std::string name = readStr(is, name_size);
  unpad(is, 4);
  std::string desc = readStr(is, desc_size);
  unpad(is, 4);

  return Note{std::move(name), std::move(desc), note_type};
}

} // namespace

std::span<const std::byte> findSection(
    std::span<const std::byte> elf,
    std::string_view name) {
#ifdef ENABLE_ELF_READER
  auto elf_hdr = reinterpret_cast<const ElfW(Ehdr)*>(elf.data());
  auto elf_ptr = reinterpret_cast<const std::byte*>(elf.data());
  std::span<const ElfW(Shdr)> section_headers{
      reinterpret_cast<const ElfW(Shdr)*>(elf_ptr + elf_hdr->e_shoff),
      elf_hdr->e_shnum};
  if (!contains(elf, section_headers)) {
    throw std::runtime_error{
        "ELF section headers are invalid, extend past the file itself"};
  }

  // Find the .shstrtab section so we can read section names.
  const ElfW(Shdr*) shstrtab_header = &section_headers[elf_hdr->e_shstrndx];
  std::span<const char> shstrtab{
      reinterpret_cast<const char*>(elf_ptr + shstrtab_header->sh_offset),
      shstrtab_header->sh_size};
  if (!contains(elf, shstrtab)) {
    throw std::runtime_error{
        ".shstrtab section is not contained within the ELF file"};
  }

  for (const ElfW(Shdr) & section_header : section_headers) {
    std::string_view section_name =
        reinterpret_cast<const char*>(shstrtab.data() + section_header.sh_name);
    if (section_name == name) {
      return std::span{
          elf_ptr + section_header.sh_offset, section_header.sh_size};
    }
  }

  return std::span<const std::byte>{};
#else
  throw std::runtime_error{"ELF reading is not supported"};
#endif
}

NoteArray readNoteSection(std::istream& is, size_t size) {
  NoteArray notes;
  // TODO: The tellg check is still needed even though there's already a check
  // for EOF.  Not sure why.
  while (is.good() && !is.eof() && is.tellg() < size) {
    notes.insert(readNote(is));
  }
  return notes;
}

NoteArray readNoteSection(std::span<const std::byte> bytes) {
  // TODO: Use std::spanstream when this moves to C++23.
  std::string copy{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  std::istringstream is{copy};
  return readNoteSection(is, bytes.size());
}

CodeNoteData parseCodeNote(const Note& note) {
  std::istringstream is{note.desc};

  auto file_name_size = readInt<uint32_t>(is);
  std::string file_name =
      readStr<false /* hasNulTerminator */>(is, file_name_size);

  auto lineno = readInt<uint32_t>(is);
  auto hash = readInt<uint32_t>(is);
  auto compiled_code_size = readInt<uint32_t>(is);
  auto normal_entry_offset = readInt<uint32_t>(is);
  auto static_entry_offset = readInt<uint32_t>(is);

  return CodeNoteData{
      file_name,
      lineno,
      hash,
      compiled_code_size,
      normal_entry_offset,
      static_entry_offset != kInvalidStaticOffset
          ? std::make_optional(static_entry_offset)
          : std::nullopt};
}

} // namespace jit::elf
