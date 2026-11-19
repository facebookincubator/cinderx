// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/config.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
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
    code_allocator_ = std::unique_ptr<CodeAllocator>(
        CodeAllocator::make(std::make_unique<CodeReservation>()));
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

TEST_F(CodeAllocatorTest, ChunksComeFromSuppliedReservation) {
  code_allocator_.reset();
  code_allocator_ = std::unique_ptr<CodeAllocator>(
      CodeAllocator::make(std::make_unique<CodeReservation>()));
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 64, result);

  EXPECT_GT(cinder->reservedBytesRemaining(), 0u);
  EXPECT_TRUE(code_allocator_->contains(result.addr));
  code_allocator_.reset();
}

TEST_F(CodeAllocatorTest, NoReservationMapsChunksIndependently) {
  code_allocator_.reset();
  code_allocator_ = std::unique_ptr<CodeAllocator>(CodeAllocator::make());
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 64, result);

  EXPECT_EQ(cinder->reservedBytesRemaining(), 0u);
  EXPECT_TRUE(code_allocator_->contains(result.addr));
}

TEST_F(CodeAllocatorTest, ZeroReservationSizeMapsChunksIndependently) {
  code_allocator_.reset();
  getMutableConfig().mem.code_reservation_size = 0;
  code_allocator_ = std::unique_ptr<CodeAllocator>(
      CodeAllocator::make(std::make_unique<CodeReservation>()));
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 64, result);

  EXPECT_EQ(cinder->reservedBytesRemaining(), 0u);
}

TEST_F(CodeAllocatorTest, FailedReservationFallsBackToIndependentChunks) {
  code_allocator_.reset();
  // Far more address space than the process has, so reserving it fails.
  getMutableConfig().mem.code_reservation_size = size_t{1} << 60;
  code_allocator_ = std::unique_ptr<CodeAllocator>(
      CodeAllocator::make(std::make_unique<CodeReservation>()));
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);

  AllocateResult result;
  addSplitTestCode(code_allocator_.get(), 64, 64, result);

  EXPECT_EQ(cinder->reservedBytesRemaining(), 0u);
  EXPECT_TRUE(code_allocator_->contains(result.addr));
}

TEST_F(CodeAllocatorTest, FailedReservationIsNotRetried) {
  code_allocator_.reset();
  code_allocator_ = std::unique_ptr<CodeAllocator>(
      CodeAllocator::make(std::make_unique<CodeReservation>()));
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);
  // Each allocation is too big to share a 2 MiB chunk with the previous one,
  // so every call needs a new chunk.
  constexpr size_t kCodeSize = 1536 * 1024;
  AllocateResult result;
  getMutableConfig().mem.code_reservation_size = size_t{1} << 60;
  addSplitTestCode(code_allocator_.get(), kCodeSize, 0, result);
  // The failed reservation is abandoned, leaving nothing reserved.
  ASSERT_EQ(cinder->reservedBytesRemaining(), 0u);

  // A size that would now succeed isn't tried.
  getMutableConfig().mem.code_reservation_size = 64 * 1024 * 1024;
  addSplitTestCode(code_allocator_.get(), kCodeSize, 0, result);

  EXPECT_EQ(cinder->reservedBytesRemaining(), 0u);
  code_allocator_.reset();
}

TEST_F(CodeAllocatorTest, ChunksStayTogetherAcrossUnrelatedMappings) {
  code_allocator_.reset();
  getMutableConfig().mem.multiple_code_sections = false;
  code_allocator_ = std::unique_ptr<CodeAllocator>(
      CodeAllocator::make(std::make_unique<CodeReservation>()));

  // Each allocation is too big to share a 2 MiB chunk with the previous one,
  // so every call needs a new chunk.
  constexpr size_t kCodeSize = 1536 * 1024;
  constexpr size_t kChunkSize = 2 * 1024 * 1024;
  auto* cinder = dynamic_cast<CodeAllocatorCinder*>(code_allocator_.get());
  ASSERT_NE(cinder, nullptr);
  AllocateResult first;
  addSplitTestCode(code_allocator_.get(), kCodeSize, 0, first);
  ASSERT_GE(cinder->reservedBytesRemaining(), kChunkSize);

  // An unrelated mapping placed by the kernel where the next chunk would go
  // if chunks were mapped independently.
  constexpr size_t kUnrelatedSize = size_t{1} << 30;
  void* unrelated = mmap(
      nullptr,
      kUnrelatedSize,
      PROT_NONE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
      -1,
      0);
  ASSERT_NE(unrelated, MAP_FAILED);

  AllocateResult second;
  addSplitTestCode(code_allocator_.get(), kCodeSize, 0, second);
  munmap(unrelated, kUnrelatedSize);

  auto distance = static_cast<size_t>(std::abs(
      reinterpret_cast<intptr_t>(second.addr) -
      reinterpret_cast<intptr_t>(first.addr)));
  EXPECT_LT(distance, kUnrelatedSize);
}

#endif

} // namespace
