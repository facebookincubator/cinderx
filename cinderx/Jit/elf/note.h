// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jit::elf {

// A note in an ELF file is a tuple of a string name, an integral type, and an
// optional descriptor string.  The type must be nonnegative and zero-length
// names are reserved by the ELF standard, but everything else is fair game.
struct Note {
  std::string name;
  std::string desc;
  uint32_t type{0};

  constexpr size_t size_bytes() const {
    // The string sizes and the type are always present.
    size_t s = sizeof(uint32_t) * 3;

    // The name is assumed to be present for our purposes, but the descriptor
    // might not be.
    s += roundUp(name.size() + 1, 4);
    if (!desc.empty()) {
      s += roundUp(desc.size() + 1, 4);
    }

    return s;
  }
};

struct NoteArray {
  template <class... Args>
  void insert(Args&&... args) {
    notes_.emplace_back(std::forward<Args>(args)...);
  }

  constexpr std::span<const Note> notes() const {
    return std::span{notes_};
  }

  constexpr size_t size_bytes() const {
    size_t sum = 0;
    for (const Note& note : notes_) {
      sum += note.size_bytes();
    }
    return sum;
  }

 private:
  std::vector<Note> notes_;
};

// CodeEntry equivalent that's encoded into an ELF note.
struct CodeNoteData {
  std::string file_name;
  uint32_t lineno{0};
  // Hash of the code stream.
  uint32_t hash{0};
  // Size of the code object, in bytes.
  uint32_t size{0};
  // Byte offset from the start of the code buffer into the normal entry point.
  uint32_t normal_entry_offset{0};
  // Byte offset from the start of the code buffer into the static entry point.
  // Only exists if this is a Static Python function.
  std::optional<uint32_t> static_entry_offset;
};

constexpr uint32_t kInvalidStaticOffset = ~uint32_t{0};

constexpr std::string_view kFuncNoteSectionName = ".note.pyfunc";

} // namespace jit::elf
