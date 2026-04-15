// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/config.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

using namespace jit;
using namespace jit::codegen;

namespace {

class CodeAllocatorTest : public ::testing::Test {
 public:
  void SetUp() override {
    saved_config_ = getConfig();
    getMutableConfig().multiple_code_sections = true;
    getMutableConfig().use_huge_pages = true;
    code_allocator_ = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  }

  void TearDown() override {
    code_allocator_.reset();
    getMutableConfig() = saved_config_;
  }

  Config saved_config_;
  std::unique_ptr<ICodeAllocator> code_allocator_;
};

const uint8_t* addressAtOffset(const void* base, uintptr_t offset) {
  return reinterpret_cast<const uint8_t*>(
      reinterpret_cast<uintptr_t>(base) + offset);
}

TEST_F(CodeAllocatorTest, AddSplitCodeCopiesHotAndColdSections) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  asmjit::Section* coldtext = nullptr;
  ASSERT_EQ(
      code.newSection(
          &coldtext,
          codeSectionName(CodeSection::kCold),
          SIZE_MAX,
          code.textSection()->flags(),
          code.textSection()->alignment()),
      asmjit::kErrorOk);

  arch::Builder as(&code);

  constexpr std::array<uint8_t, 3> kHotText{{0x11, 0x12, 0x13}};
  constexpr std::array<uint8_t, 5> kColdText{{0x31, 0x32, 0x33, 0x34, 0x35}};

  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kHotText.data(), kHotText.size()), asmjit::kErrorOk);

  ASSERT_EQ(as.section(coldtext), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kColdText.data(), kColdText.size()), asmjit::kErrorOk);

  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);
  ASSERT_NE(result.addr, kHotText.data());
  ASSERT_NE(result.addr, kColdText.data());

  ASSERT_EQ(code.textSection()->offset(), 0);

  EXPECT_EQ(
      std::memcmp(
          addressAtOffset(result.addr, code.textSection()->offset()),
          kHotText.data(),
          kHotText.size()),
      0);
  EXPECT_EQ(
      std::memcmp(
          addressAtOffset(result.addr, coldtext->offset()),
          kColdText.data(),
          kColdText.size()),
      0);
}

} // namespace
