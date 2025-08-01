// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/config.h"

#include <asmjit/asmjit.h>

#include <cstring>
#include <vector>

namespace jit::codegen {

enum class CodeSection {
  kHot,
  kCold,
};

const char* codeSectionName(CodeSection section);

// Provides a reverse mapping from text section names to CodeSection enums. Will
// abort if a section is unknown.
CodeSection codeSectionFromName(const char* name);

class CodeHolderMetadata {
 public:
  explicit CodeHolderMetadata(CodeSection section) : section_(section) {}

  void setSection(CodeSection section) {
    section_ = section;
  }

 private:
  friend class CodeSectionOverride;

  CodeSection section_;
};

// RAII device for overriding the previous code section.
class CodeSectionOverride {
 public:
  CodeSectionOverride() = delete;

  CodeSectionOverride(
      asmjit::x86::Builder* as,
      const asmjit::CodeHolder* code,
      CodeHolderMetadata* metadata,
      CodeSection section)
      : as_{as}, code_{code}, metadata_{metadata} {
    if (getConfig().multiple_code_sections) {
      previous_section_ = metadata->section_;
      metadata->section_ = section;
      as->section(code->sectionByName(codeSectionName(section)));
    } else {
      previous_section_ = section;
    }
  }

  ~CodeSectionOverride() {
    // Guard against partial initialization to make GCC happy.
    if (as_ == nullptr || code_ == nullptr) {
      return;
    }
    if (getConfig().multiple_code_sections) {
      as_->section(code_->sectionByName(codeSectionName(previous_section_)));
      metadata_->section_ = previous_section_;
    }
  }

 private:
  asmjit::x86::Builder* as_;
  const asmjit::CodeHolder* code_;
  CodeSection previous_section_;
  CodeHolderMetadata* metadata_;
};

// Call f with each code section.
template <typename F>
void forEachSection(F f) {
  f(CodeSection::kHot);
  f(CodeSection::kCold);
}

void populateCodeSections(
    std::vector<std::pair<void*, std::size_t>>& output_vector,
    asmjit::CodeHolder& code,
    void* entry);

} // namespace jit::codegen
