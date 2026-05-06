// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"

#include <cstdint>
#include <memory>

#if defined(CINDER_AARCH64)

using namespace jit;
using namespace jit::codegen;

namespace {

class BranchRelaxationTest : public ::testing::Test {
 public:
  void SetUp() override {
    code_allocator_ = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  }

  void TearDown() override {
    code_allocator_.reset();
  }

  void* compileBuilder(arch::Builder& as, asmjit::CodeHolder& code) {
    EXPECT_EQ(as.finalize(), asmjit::kErrorOk);
    AllocateResult result = code_allocator_->addCode(&code);
    EXPECT_EQ(result.error, asmjit::kErrorOk);
    return result.addr;
  }

  size_t finalizeAndGetSize(arch::Builder& as, asmjit::CodeHolder& code) {
    EXPECT_EQ(as.finalize(), asmjit::kErrorOk);
    return code.textSection()->bufferSize();
  }

  size_t countNops(asmjit::CodeHolder& code) {
    constexpr uint32_t kAArch64Nop = 0xD503201Fu;
    asmjit::Section* text = code.textSection();
    const uint8_t* buf = text->data();
    size_t size = text->bufferSize();
    size_t count = 0;
    for (size_t i = 0; i + 3 < size; i += 4) {
      uint32_t inst;
      memcpy(&inst, buf + i, 4);
      if (inst == kAArch64Nop)
        count++;
    }
    return count;
  }

  std::unique_ptr<ICodeAllocator> code_allocator_;
};

TEST_F(BranchRelaxationTest, InRangeCondBranch) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.cbz(asmjit::a64::x0, target);
  as.mov(asmjit::a64::x0, 42);
  as.bind(target);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(0), 0u);
  EXPECT_EQ(func(1), 42u);
}

// tbz has a 14-bit signed immediate (+/-32KB).
TEST_F(BranchRelaxationTest, OutOfRangeTbzIsRelaxed) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.tbz(asmjit::a64::x0, 0, target);

  for (int i = 0; i < 9000; i++) {
    as.nop();
  }

  as.mov(asmjit::a64::x0, 1);
  as.ret(asmjit::a64::x30);

  as.bind(target);
  as.mov(asmjit::a64::x0, 0);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(0), 0u);
  EXPECT_EQ(func(1), 1u);
  EXPECT_EQ(func(2), 0u);
  EXPECT_EQ(func(3), 1u);
}

// cbz has a 19-bit signed immediate (+/-1MB).
TEST_F(BranchRelaxationTest, OutOfRangeCbzIsRelaxed) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.cbz(asmjit::a64::x0, target);

  for (int i = 0; i < 270000; i++) {
    as.nop();
  }

  as.mov(asmjit::a64::x0, 1);
  as.ret(asmjit::a64::x30);

  as.bind(target);
  as.mov(asmjit::a64::x0, 0);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(0), 0u);
  EXPECT_EQ(func(1), 1u);
}

TEST_F(BranchRelaxationTest, OutOfRangeCondBranchIsRelaxed) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.cmp(asmjit::a64::x0, 0);
  as.b_eq(target);

  for (int i = 0; i < 270000; i++) {
    as.nop();
  }

  as.mov(asmjit::a64::x0, 1);
  as.ret(asmjit::a64::x30);

  as.bind(target);
  as.mov(asmjit::a64::x0, 0);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(0), 0u);
  EXPECT_EQ(func(1), 1u);
}

TEST_F(BranchRelaxationTest, OutOfRangeBackwardTbzIsRelaxed) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label loop_top = as.newLabel();
  asmjit::Label done = as.newLabel();

  as.b(done);

  as.bind(loop_top);
  as.nop();

  for (int i = 0; i < 9000; i++) {
    as.nop();
  }

  as.tbz(asmjit::a64::x0, 0, loop_top);

  as.bind(done);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(42), 42u);
}

TEST_F(BranchRelaxationTest, NoNopPaddingForInRangeBranches) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.cbz(asmjit::a64::x0, target);
  as.mov(asmjit::a64::x0, 42);
  as.bind(target);
  as.ret(asmjit::a64::x30);

  // cbz(4) + mov(4) + ret(4) = 12 bytes, no NOP padding.
  EXPECT_EQ(finalizeAndGetSize(as, code), 12u);
  EXPECT_EQ(countNops(code), 0u);
}

TEST_F(BranchRelaxationTest, MultipleForwardBranchesNoNops) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label l1 = as.newLabel();
  asmjit::Label l2 = as.newLabel();
  asmjit::Label l3 = as.newLabel();

  as.cbz(asmjit::a64::x0, l1);
  as.tbz(asmjit::a64::x0, 1, l2);
  as.cmp(asmjit::a64::x0, 0);
  as.b_eq(l3);
  as.mov(asmjit::a64::x0, 99);
  as.bind(l1);
  as.bind(l2);
  as.bind(l3);
  as.ret(asmjit::a64::x30);

  // 6 instructions * 4 bytes = 24 bytes, no NOP padding.
  EXPECT_EQ(finalizeAndGetSize(as, code), 24u);
  EXPECT_EQ(countNops(code), 0u);
}

// Unconditional b to a cross-section label resolves correctly.
TEST_F(BranchRelaxationTest, UnconditionalBranchCrossSection) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  asmjit::Section* cold;
  ASSERT_EQ(
      code.newSection(
          &cold,
          ".cold",
          SIZE_MAX,
          code.textSection()->flags(),
          code.textSection()->alignment()),
      asmjit::kErrorOk);

  arch::Builder as(&code);

  asmjit::Label cold_target = as.newLabel();

  // Hot section: branch to cold, then return 1 (never reached).
  as.b(cold_target);
  as.mov(asmjit::a64::x0, 1);
  as.ret(asmjit::a64::x30);

  // Cold section: return 42.
  as.section(cold);
  as.bind(cold_target);
  as.mov(asmjit::a64::x0, 42);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)()>(fn);
  EXPECT_EQ(func(), 42u);
}

// Conditional branch relaxation works across sections: the expanded
// unconditional b targets the cross-section label.
TEST_F(BranchRelaxationTest, CondBranchCrossSectionIsRelaxed) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  asmjit::Section* cold;
  ASSERT_EQ(
      code.newSection(
          &cold,
          ".cold",
          SIZE_MAX,
          code.textSection()->flags(),
          code.textSection()->alignment()),
      asmjit::kErrorOk);

  arch::Builder as(&code);

  asmjit::Label cold_target = as.newLabel();

  // Hot section: conditional branch to cold section.
  as.cbz(asmjit::a64::x0, cold_target);
  as.mov(asmjit::a64::x0, 1);
  as.ret(asmjit::a64::x30);

  // Cold section: return 0.
  as.section(cold);
  as.bind(cold_target);
  as.mov(asmjit::a64::x0, 0);
  as.ret(asmjit::a64::x30);

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)(uint64_t)>(fn);
  EXPECT_EQ(func(0), 0u);
  EXPECT_EQ(func(1), 1u);
}

// adr and ldr literal use 8-byte RelocEntry encoding to allow relaxation
// to adrp+add / adrp+ldr for large displacements at relocation time.
TEST_F(BranchRelaxationTest, AdrForwardRefUsesReloc) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label target = as.newLabel();

  as.adr(asmjit::a64::x0, target);
  as.ret(asmjit::a64::x30);
  as.bind(target);
  as.nop();

  // adr(4) + nop(4) + ret(4) + nop(4) = 16 bytes.
  EXPECT_EQ(finalizeAndGetSize(as, code), 16u);
}

TEST_F(BranchRelaxationTest, LdrLiteralForwardRefUsesReloc) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label pool_entry = as.newLabel();

  as.ldr(asmjit::a64::x0, asmjit::a64::ptr(pool_entry));
  as.ret(asmjit::a64::x30);
  as.bind(pool_entry);
  uint64_t value = 0xDEADBEEFCAFEBABEull;
  as.embed(&value, sizeof(value));

  // ldr(4) + nop(4) + ret(4) + data(8) = 20 bytes.
  EXPECT_EQ(finalizeAndGetSize(as, code), 20u);
}

TEST_F(BranchRelaxationTest, LdrLiteralLoadsCorrectValue) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());
  arch::Builder as(&code);

  asmjit::Label pool_entry = as.newLabel();

  as.ldr(asmjit::a64::x0, asmjit::a64::ptr(pool_entry));
  as.ret(asmjit::a64::x30);
  as.bind(pool_entry);
  uint64_t value = 42;
  as.embed(&value, sizeof(value));

  void* fn = compileBuilder(as, code);
  ASSERT_NE(fn, nullptr);

  auto func = reinterpret_cast<uint64_t (*)()>(fn);
  EXPECT_EQ(func(), 42u);
}

} // namespace

#endif // CINDER_AARCH64
