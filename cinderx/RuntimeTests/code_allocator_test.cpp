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
#include <vector>

using namespace cinderx::jit;
using namespace cinderx::jit::codegen;

namespace {

class CodeAllocatorTest : public ::testing::Test {
 public:
  void SetUp() override {
    saved_config_ = getConfig();
    getMutableConfig().mem.multiple_code_sections = true;
    getMutableConfig().mem.huge_pages = true;
    code_allocator_ = std::unique_ptr<CodeAllocator>(CodeAllocator::make());
  }

  void TearDown() override {
    code_allocator_.reset();
    getMutableConfig() = saved_config_;
  }

  Config saved_config_;
  std::unique_ptr<CodeAllocator> code_allocator_;
};

const uint8_t* addressAtOffset(const void* base, uintptr_t offset) {
  return reinterpret_cast<const uint8_t*>(
      reinterpret_cast<uintptr_t>(base) + offset);
}

#if defined(__linux__) && defined(__ELF__)

// Allocate `hot_size` bytes of hot code and `cold_size` bytes of cold code
// (all NOPs) through `allocator`, as a stand-in for a JIT-compiled
// function with split code sections.
void addSplitTestCode(
    CodeAllocator* allocator,
    size_t hot_size,
    size_t cold_size,
    AllocateResult& result) {
  asmjit::CodeHolder code;
  ASSERT_EQ(code.init(allocator->asmJitEnvironment()), asmjit::kErrorOk);

  asmjit::Section* coldtext = nullptr;
  if (cold_size > 0) {
    ASSERT_EQ(
        code.newSection(
            &coldtext,
            codeSectionName(CodeSection::kCold),
            SIZE_MAX,
            code.textSection()->flags(),
            code.textSection()->alignment()),
        asmjit::kErrorOk);
  }

  arch::Builder as(&code);
  const std::vector<uint8_t> hot_text(hot_size, 0x90);
  const std::vector<uint8_t> cold_text(cold_size, 0x90);

  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(hot_text.data(), hot_text.size()), asmjit::kErrorOk);

  if (coldtext != nullptr) {
    ASSERT_EQ(as.section(coldtext), asmjit::kErrorOk);
    ASSERT_EQ(as.embed(cold_text.data(), cold_text.size()), asmjit::kErrorOk);
  }

  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  result = allocator->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);
}

#endif

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

// Indirect calls built with -fsanitize=function read the 8 bytes in front of
// their target, so the memory there has to belong to the allocator.
void expectReadablePrefix(const CodeAllocator& allocator, const void* addr) {
  EXPECT_TRUE(allocator.contains(static_cast<const uint8_t*>(addr) - 8))
      << "code at " << addr << " starts at the beginning of a mapping";
}

TEST_F(CodeAllocatorTest, AddSplitCodeLeavesReadablePrefix) {
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
  constexpr std::array<uint8_t, 3> kText{{0x11, 0x12, 0x13}};
  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.section(coldtext), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  expectReadablePrefix(*code_allocator_, result.addr);
  expectReadablePrefix(
      *code_allocator_, addressAtOffset(result.addr, coldtext->offset()));
}

TEST_F(CodeAllocatorTest, AddCodeLeavesReadablePrefix) {
  code_allocator_.reset();
  getMutableConfig().mem.multiple_code_sections = false;
  code_allocator_ = std::unique_ptr<CodeAllocator>(CodeAllocator::make());

  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);
  constexpr std::array<uint8_t, 3> kText{{0x11, 0x12, 0x13}};
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);
  expectReadablePrefix(*code_allocator_, result.addr);
}

#if defined(__linux__) && defined(__ELF__)

TEST_F(CodeAllocatorTest, ChunkUnwindRegistrationIsSharedAcrossFunctions) {
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 64, result);
  size_t count = cinder->chunkUnwindRegistrationCount();
  EXPECT_GT(count, 0u);

  // A second function in the same chunk(s) adds no registration: the
  // chunk frame already covers it.
  addSplitTestCode(code_allocator_.get(), 64, 64, result);
  EXPECT_EQ(cinder->chunkUnwindRegistrationCount(), count);

  // A function too large for the remaining space allocates new
  // chunk(s), each registered once.
  constexpr size_t kLargeSize = 3 * 1024 * 1024;
  addSplitTestCode(code_allocator_.get(), kLargeSize, kLargeSize, result);
  EXPECT_GT(cinder->chunkUnwindRegistrationCount(), count);
}

TEST_F(CodeAllocatorTest, ChunkUnwindRegistrationIsSharedWithoutSplitSections) {
  code_allocator_.reset();
  getMutableConfig().mem.multiple_code_sections = false;
  code_allocator_ = std::unique_ptr<CodeAllocator>(CodeAllocator::make());
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 0, result);
  size_t count = cinder->chunkUnwindRegistrationCount();
  EXPECT_GT(count, 0u);

  addSplitTestCode(code_allocator_.get(), 64, 0, result);
  EXPECT_EQ(cinder->chunkUnwindRegistrationCount(), count);
}

TEST_F(CodeAllocatorTest, FunctionUnwindRegistrationsAreReleasedWithCode) {
  code_allocator_.reset();
  getMutableConfig().mem.huge_pages = false;
  code_allocator_ = std::unique_ptr<CodeAllocator>(CodeAllocator::make());
  auto* fallback = dynamic_cast<CodeAllocator*>(code_allocator_.get());
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get()), nullptr);

  AllocateResult first;
  addSplitTestCode(code_allocator_.get(), 64, 64, first);
  // One registration per section: hot and cold.
  EXPECT_EQ(fallback->unwindRegistrationCount(), 2u);

  AllocateResult second;
  addSplitTestCode(code_allocator_.get(), 64, 64, second);
  EXPECT_EQ(fallback->unwindRegistrationCount(), 4u);

  ASSERT_EQ(code_allocator_->releaseCode(first.addr), asmjit::kErrorOk);
  EXPECT_EQ(fallback->unwindRegistrationCount(), 2u);

  ASSERT_EQ(code_allocator_->releaseCode(second.addr), asmjit::kErrorOk);
  EXPECT_EQ(fallback->unwindRegistrationCount(), 0u);
}

#endif

} // namespace
