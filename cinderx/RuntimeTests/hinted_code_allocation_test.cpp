// Copyright (c) Meta Platforms, Inc. and affiliates.

// End-to-end test that JIT code allocation uses the linker-reserved
// __cinder_jit region. This binary is linked with the production linker script
// (//cinderx:linker_script_ld) which reserves a `.cinder_jit (NOLOAD)` region
// immediately around .text, delimited by the weak __cinder_jit_start/end
// symbols. With hinted_code_allocation enabled, CodeAllocatorCinder::addCode
// should bump-allocate out of that region, so the returned code address must
// fall within [__cinder_jit_start, __cinder_jit_end). This exercises the
// mprotect-based path in code_allocator.cpp with a real linker script.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/config.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace cinderx::jit;

namespace {

#if defined(__linux__)
// The linker script reserves the JIT region and delimits it with these weak
// symbols. They resolve to nullptr if the binary wasn't linked with the script.
extern "C" {
extern char __cinder_jit_start[] __attribute__((weak));
extern char __cinder_jit_end[] __attribute__((weak));
}
#endif

class HintedCodeAllocationTest : public ::testing::Test {
 public:
  void SetUp() override {
    saved_config_ = getConfig();
    // use_huge_pages selects CodeAllocatorCinder, whose addCode() routes
    // through allocPages(); hinted_code_allocation makes allocPages() prefer
    // the linker-reserved __cinder_jit region.
    getMutableConfig().use_huge_pages = true;
    getMutableConfig().hinted_code_allocation = true;
    getMutableConfig().multiple_code_sections = false;
    code_allocator_ = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  }

  void TearDown() override {
    code_allocator_.reset();
    getMutableConfig() = saved_config_;
  }

  Config saved_config_;
  std::unique_ptr<ICodeAllocator> code_allocator_;
};

TEST_F(HintedCodeAllocationTest, AllocatesWithinCinderJitRegion) {
#if defined(__linux__)
  // The whole point of this binary is that it's linked with the script that
  // reserves the region; if these are null the test isn't testing anything.
  char* region_start = __cinder_jit_start;
  char* region_end = __cinder_jit_end;

  if (region_start == nullptr) {
    GTEST_SKIP() << "binary not linked with the .cinder_jit linker script";
  }

  ASSERT_NE(region_end, nullptr)
      << "binary not linked with the .cinder_jit linker script";
  ASSERT_LT(region_start, region_end);

  // Build a trivial chunk of code so addCode() has something to allocate for.
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  codegen::arch::Builder as(&code);
  constexpr std::array<uint8_t, 4> kText{{0x90, 0x90, 0x90, 0xc3}};
  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);

  // The allocated code must live inside the reserved region.
  char* addr = static_cast<char*>(result.addr);
  EXPECT_GE(addr, region_start);
  EXPECT_LT(addr, region_end);
#else
  GTEST_SKIP() << "cinder_jit region allocation is only supported on Linux";
#endif
}

// A request the linker-reserved region can't satisfy must fall back to mmap
// rather than handing back a pointer past the end of the region.
TEST_F(HintedCodeAllocationTest, FallsBackWhenRequestExceedsRegion) {
#if defined(__linux__)
  char* region_start = __cinder_jit_start;
  char* region_end = __cinder_jit_end;

  if (region_start == nullptr) {
    GTEST_SKIP() << "binary not linked with the .cinder_jit linker script";
  }
  ASSERT_NE(region_end, nullptr)
      << "binary not linked with the .cinder_jit linker script";
  ASSERT_LT(region_start, region_end);

  // Build a code blob larger than the whole region so the bump allocator can
  // never satisfy it, forcing the mmap fallback. 0x90 is a nop on x86-64; the
  // exact bytes don't matter, only that they're really copied into the
  // allocation (exercising the memcpy that crashed in production).
  size_t region_size = static_cast<size_t>(region_end - region_start);
  std::vector<uint8_t> blob(region_size + 4096, 0x90);
  blob.back() = 0xc3; // ret

  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  cinderx::jit::codegen::arch::Builder as(&code);
  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(blob.data(), blob.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);

  // The allocation must have come from the mmap fallback, outside the region.
  char* addr = static_cast<char*>(result.addr);
  EXPECT_TRUE(addr < region_start || addr >= region_end)
      << "oversized request was satisfied from inside the reserved region";
#else
  GTEST_SKIP() << "cinder_jit region allocation is only supported on Linux";
#endif
}

} // namespace
