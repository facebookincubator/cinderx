// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/code_section.h"

#include "cinderx/Common/log.h"

#include <cstdint>

namespace cinderx::jit::codegen {
const char* codeSectionName(CodeSection section) {
  switch (section) {
    case CodeSection::kHot:
      return ".text";
    case CodeSection::kCold:
      return ".coldtext";
  }
  JIT_ABORT("Bad code section {}", static_cast<int>(section));
}

CodeSection codeSectionFromName(const char* name) {
  if (strcmp(name, ".text") == 0 || strcmp(name, ".addrtab") == 0 ||
      strcmp(name, ".a64stubs") == 0) {
    return CodeSection::kHot;
  }
  if (strcmp(name, ".coldtext") == 0) {
    return CodeSection::kCold;
  }
  JIT_ABORT("Bad code section name {}", name);
}

void populateCodeSections(
    std::vector<std::pair<void*, std::size_t>>& code_sections,
    asmjit::CodeHolder& code,
    void* code_base_ptr) {
  for (const asmjit::Section* section : code.sectionsByOrder()) {
    if (!section->hasFlag(asmjit::SectionFlags::kExecutable) ||
        section->realSize() == 0) {
      continue;
    }
    // Section offsets are relative to the code base.  With split code
    // sections the cold offset is the (possibly negative) displacement
    // between two separate allocations, stored as an unsigned value, so
    // adding it to the base as pointer arithmetic overflows even though
    // the wrapped result is the cold section's address.  Do the
    // arithmetic on integers, where wraparound is defined, instead.
    auto section_start = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(code_base_ptr) + section->offset());
    code_sections.emplace_back(
        reinterpret_cast<void*>(section_start), section->realSize());
  }
}

} // namespace cinderx::jit::codegen
