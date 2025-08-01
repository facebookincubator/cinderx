// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/elf/note.h"

#include <cstddef>
#include <iosfwd>

namespace jit::elf {

// Find an ELF section by name from an ELF file.
//
// Returns an empty span if the section cannot be found.
std::span<const std::byte> findSection(
    std::span<const std::byte> elf,
    std::string_view name);

// Read the ELF notes out of an ELF section.
NoteArray readNoteSection(std::istream& is, size_t size);
NoteArray readNoteSection(std::span<const std::byte> bytes);

// Parse a function's code note data out of an ELF note.
CodeNoteData parseCodeNote(const Note& note);

} // namespace jit::elf
