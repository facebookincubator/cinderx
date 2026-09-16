// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstddef>
#include <vector>

using namespace cinderx::jit::codegen;

namespace cinderx::jit::codegen {

class CodegenTest : public RuntimeTest {};

TEST_F(CodegenTest, TestPhyRegisterSet) {
  auto set = PhyRegisterSet(2) | PhyRegisterSet(3) | PhyRegisterSet(5);

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 3);
  ASSERT_EQ(set.getFirst(), 2);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_EQ(set.has(3), true);

  set.removeFirst();

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 2);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_EQ(set.has(3), true);

  set.removeLast();

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 1);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 3);
  ASSERT_EQ(set.has(3), true);

  set.removeFirst();

  ASSERT_EQ(set.empty(), true);
  ASSERT_EQ(set.count(), 0);
  ASSERT_EQ(set.has(3), false);
}

TEST_F(CodegenTest, PopulateCodeSectionsIncludesNonemptyExecutableSections) {
  asmjit::CodeHolder code;
  ASSERT_EQ(code.init(asmjit::Environment::host()), asmjit::kErrorOk);

  asmjit::Section* text = code.textSection();
  text->setVirtualSize(4);

  asmjit::Section* executable;
  ASSERT_EQ(
      code.newSection(
          &executable,
          ".extra_text",
          SIZE_MAX,
          asmjit::SectionFlags::kExecutable | asmjit::SectionFlags::kReadOnly),
      asmjit::kErrorOk);
  executable->setVirtualSize(8);

  asmjit::Section* data;
  ASSERT_EQ(
      code.newSection(
          &data, ".data", SIZE_MAX, asmjit::SectionFlags::kReadOnly),
      asmjit::kErrorOk);
  data->setVirtualSize(16);

  asmjit::Section* empty_executable;
  ASSERT_EQ(
      code.newSection(
          &empty_executable,
          ".empty_text",
          SIZE_MAX,
          asmjit::SectionFlags::kExecutable | asmjit::SectionFlags::kReadOnly),
      asmjit::kErrorOk);

  ASSERT_EQ(code.flatten(), asmjit::kErrorOk);
  std::vector<std::byte> storage(code.codeSize());
  std::vector<std::pair<void*, std::size_t>> sections;
  populateCodeSections(sections, code, storage.data());

  const std::vector<std::pair<void*, std::size_t>> expected{
      {storage.data() + text->offset(), text->realSize()},
      {storage.data() + executable->offset(), executable->realSize()},
  };
  EXPECT_EQ(sections, expected);
}

} // namespace cinderx::jit::codegen
