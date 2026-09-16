// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/code_section.h"

#include "cinderx/Common/log.h"

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
    auto section_start = static_cast<char*>(code_base_ptr) + section->offset();
    code_sections.emplace_back(
        reinterpret_cast<void*>(section_start), section->realSize());
  }
}

} // namespace cinderx::jit::codegen
