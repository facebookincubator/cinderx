// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/tls.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace cinderx::jit::codegen;

namespace cinderx::jit::codegen {

class CodegenTest : public RuntimeTest {};

TEST_F(CodegenTest, TestPhyRegisterSet) {
  auto set = PhyRegisterSet(2) | PhyRegisterSet(3) | PhyRegisterSet(5);

  ASSERT_FALSE(set.empty());
  ASSERT_EQ(set.count(), 3);
  ASSERT_EQ(set.getFirst(), 2);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_TRUE(set.has(2));
  ASSERT_TRUE(set.has(3));
  ASSERT_FALSE(set.has(4));
  ASSERT_TRUE(set.has(5));

  set.removeFirst();

  ASSERT_FALSE(set.empty());
  ASSERT_EQ(set.count(), 2);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_FALSE(set.has(2));
  ASSERT_TRUE(set.has(3));
  ASSERT_TRUE(set.has(5));

  set.removeLast();

  ASSERT_FALSE(set.empty());
  ASSERT_EQ(set.count(), 1);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 3);
  ASSERT_TRUE(set.has(3));
  ASSERT_FALSE(set.has(5));

  set.removeFirst();

  ASSERT_TRUE(set.empty());
  ASSERT_EQ(set.count(), 0);
  ASSERT_FALSE(set.has(3));
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

#if defined(CINDER_X86_64)

namespace {

// The page size emitStackAlloc() walks in.  Spelled out rather than shared so
// the test fails if the emitter's notion of a page changes silently.
constexpr int kProbePageSize = 4096;

struct StackAllocShape {
  std::vector<int> subs;
  int probes{0};
};

// Emit a stack allocation and report the `sub rsp` immediates and the number
// of `or qword [rsp], 0` probes between them.
StackAllocShape emitStackAllocShape(int bytes) {
  asmjit::CodeHolder code;
  code.init(asmjit::Environment::host());
  arch::Builder as{&code};

  autogen::emitStackAlloc(&as, bytes);

  StackAllocShape shape;
  for (asmjit::BaseNode* node = as.firstNode(); node != nullptr;
       node = node->next()) {
    if (!node->isInst()) {
      continue;
    }
    auto* inst = node->as<asmjit::InstNode>();
    if (inst->id() == asmjit::x86::Inst::kIdSub) {
      shape.subs.push_back(
          static_cast<int>(inst->op(1).as<asmjit::Imm>().value()));
    } else if (inst->id() == asmjit::x86::Inst::kIdOr) {
      shape.probes++;
    }
  }
  return shape;
}

// Only Windows commits stack pages lazily behind a guard page.
constexpr bool kWalksPages = kOS == OS::kWindows;

} // namespace

TEST(StackAllocTest, NothingIsEmittedForAnEmptyFrame) {
  EXPECT_TRUE(emitStackAllocShape(0).subs.empty());
  EXPECT_TRUE(emitStackAllocShape(-8).subs.empty());
}

TEST(StackAllocTest, AFrameWithinAPageIsASingleSub) {
  // Nothing can be skipped over, so there is no reason to walk.
  for (int bytes : {8, 512, kProbePageSize}) {
    StackAllocShape shape = emitStackAllocShape(bytes);
    EXPECT_EQ(shape.subs, std::vector<int>{bytes}) << "for " << bytes;
    EXPECT_EQ(shape.probes, 0) << "for " << bytes;
  }
}

TEST(StackAllocTest, AFrameSpanningPagesTouchesEachOne) {
  constexpr int kRemainder = 1600;
  constexpr int kBytes = 2 * kProbePageSize + kRemainder;

  StackAllocShape shape = emitStackAllocShape(kBytes);

  if (kWalksPages) {
    // One step per page, each followed by a write that lands on it, then the
    // rest.  Reaching the last page without touching the ones above it is the
    // bug this guards.
    EXPECT_EQ(
        shape.subs,
        (std::vector<int>{kProbePageSize, kProbePageSize, kRemainder}));
    EXPECT_EQ(shape.probes, 2);
  } else {
    EXPECT_EQ(shape.subs, std::vector<int>{kBytes});
    EXPECT_EQ(shape.probes, 0);
  }
}

TEST(StackAllocTest, TheWalkCoversTheWholeFrame) {
  for (int bytes : {kProbePageSize + 1, 9792, 64 * 1024}) {
    StackAllocShape shape = emitStackAllocShape(bytes);
    int total = 0;
    for (int sub : shape.subs) {
      total += sub;
    }
    EXPECT_EQ(total, bytes) << "for " << bytes;
    if (kWalksPages) {
      // More than a page, so it has to be more than one step -- without this
      // the probe count below is satisfied by not walking at all.
      EXPECT_GT(shape.subs.size(), 1u) << "for " << bytes;
      // Every step but the last is a whole page, so each has a probe.
      EXPECT_EQ(shape.probes, static_cast<int>(shape.subs.size()) - 1)
          << "for " << bytes;
    }
  }
}

#endif

TEST_F(CodegenTest, TakeProfileCollectorTwiceThrows) {
  Environ env;
  ASSERT_NE(env.takeProfileCollector(), nullptr);
  EXPECT_THROW(env.takeProfileCollector(), std::runtime_error);
  EXPECT_THROW(env.profileCollector(), std::runtime_error);
}

#if defined(CINDER_AARCH64)
TEST_F(CodegenTest, LargeOffsetMemoryLoadsAndStores) {
  using namespace asmjit;
  using namespace cinderx::jit::lir;
  struct AddressMode {
    bool indirect;
    bool sp_relative;
    bool immediate;
  };
  constexpr AddressMode modes[] = {
      {false, true, false},
      {false, true, true},
      {false, false, false},
      {false, false, true},
      {true, false, false},
      {true, false, true}};
  constexpr DataType types[] = {
      DataType::k8bit, DataType::k16bit, DataType::k32bit, DataType::k64bit};
  constexpr uint64_t value = 0xa1b2c3d4e5f6071;
  constexpr int frame_size = 65536;
  for (AddressMode mode : modes) {
    for (DataType type : types) {
      for (int offset : {18416, 32768}) {
        SCOPED_TRACE(offset);
        CodeHolder code;
        ASSERT_EQ(code.init(Environment::host()), kErrorOk);
        arch::Builder as{&code};
        Environ env;
        env.as = &as;
        env.sp_to_fp_delta =
            mode.sp_relative ? frame_size : arch::kSpPositionUnknown;
        as.stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -16));
        as.mov(a64::x29, a64::sp);
        as.mov(a64::x13, frame_size);
        as.sub(a64::sp, a64::sp, a64::x13);
        as.mov(a64::x2, a64::x0);
        as.mov(a64::x4, a64::sp);
        int loc = mode.sp_relative ? offset - frame_size : -offset;
        as.mov(a64::x5, mode.indirect ? offset : loc);
        as.add(a64::x5, mode.indirect ? a64::x4 : a64::x29, a64::x5);
        as.mov(a64::x3, UINT64_MAX);
        as.str(a64::x3, a64::ptr(a64::x5));
        as.mov(a64::x1, value);

        Function function;
        BasicBlock* block = function.allocateBasicBlock();
        Instruction* store = mode.indirect
            ? block->allocateInstr(
                  Opcode::kStore, nullptr, OutInd{X4, offset, type})
            : block->allocateInstr(
                  Opcode::kStore, nullptr, OutStk{PhyLocation{loc}, type});
        if (mode.immediate) {
          store->allocateImmediateInput(value);
        } else {
          store->allocatePhyRegisterInput(X1)->setDataType(type);
        }
        autogen::AutoTranslator::getInstance().translateInstr(&env, store);
        auto emit_load = [&](PhyLocation reg, DataType data_type) {
          Instruction* load = mode.indirect
              ? block->allocateInstr(
                    Opcode::kLoad,
                    nullptr,
                    OutPhyReg{reg, data_type},
                    Ind{X4, offset, data_type})
              : block->allocateInstr(
                    Opcode::kLoad,
                    nullptr,
                    OutPhyReg{reg, data_type},
                    Stk{PhyLocation{loc}, data_type});
          autogen::AutoTranslator::getInstance().translateInstr(&env, load);
        };
        emit_load(X1, type);
        as.str(a64::x1, a64::ptr(a64::x2));
        emit_load(X0, DataType::k64bit);
        as.mov(a64::sp, a64::x29);
        as.ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 16));
        as.ret(a64::x30);
        ASSERT_EQ(as.finalize(), kErrorOk);
        JitRuntime runtime;
        uint64_t (*fn)(uint64_t*) = nullptr;
        ASSERT_EQ(runtime.add(&fn, &code), kErrorOk);
        const unsigned bits = 8u << byteShift(type);
        const uint64_t mask = UINT64_MAX >> (64 - bits);
        uint64_t loaded = 0;
        EXPECT_EQ(fn(&loaded), (UINT64_MAX & ~mask) | (value & mask));
        EXPECT_EQ(loaded, value & mask);
        EXPECT_EQ(runtime.release(fn), kErrorOk);
      }
    }
  }
}
TEST_F(CodegenTest, IndirectMemoryStoresPreserveRegisters) {
  using namespace asmjit;
  using namespace cinderx::jit::lir;
  struct AddressCase {
    PhyLocation value;
    PhyLocation base;
    PhyLocation index;
    int32_t offset;
    uint8_t multiplier;
    bool immediate{false};
    bool zero{false};
  };
  const AddressCase cases[] = {
      {X1, X4, PhyLocation::REG_INVALID, 8192, 0},
      {X13, X4, PhyLocation::REG_INVALID, 8192, 0},
      {X14, X4, PhyLocation::REG_INVALID, 18416, 0},
      {X13, X4, X14, 8, 3},
      {X14, X13, X4, 8192, 1},
      {X1, X13, X14, 18416, 2},
      {X1, X14, X13, -8192, 3},
      {X13, X4, X14, 0, 3},
      {X13, X4, X14, 0, 0},
      {PhyLocation::REG_INVALID, X14, PhyLocation::REG_INVALID, 8192, 0, true},
      {PhyLocation::REG_INVALID, X4, X13, 8, 3, true},
      {PhyLocation::REG_INVALID, X13, X4, -8192, 2, true},
      {PhyLocation::REG_INVALID, X13, X14, 0, 0, true, true},
      {PhyLocation::REG_INVALID, X13, X4, 18416, 3, true, true},
  };
  constexpr DataType types[] = {
      DataType::k8bit, DataType::k16bit, DataType::k32bit, DataType::k64bit};
  constexpr uint64_t value = 0xa1b2c3d4e5f6071;
  uint64_t memory[8192] = {};
  for (const AddressCase& test_case : cases) {
    for (DataType type : types) {
      SCOPED_TRACE(
          ::testing::Message()
          << test_case.value.loc << ":" << test_case.base.loc << ":"
          << test_case.index.loc << ":" << test_case.offset << ":"
          << bitSize(type));
      CodeHolder code;
      ASSERT_EQ(code.init(Environment::host()), kErrorOk);
      arch::Builder as{&code};
      Environ env;
      env.as = &as;
      as.mov(a64::x2, a64::x1);
      auto base = a64::x(test_case.base.loc);
      as.add(base, a64::x0, 32768);
      const bool indexed = test_case.index != PhyLocation::REG_INVALID;
      if (indexed) {
        as.mov(a64::x(test_case.index.loc), 5);
      }
      const uint64_t stored_value = test_case.zero ? 0 : value;
      if (!test_case.immediate) {
        as.mov(a64::x(test_case.value.loc), stored_value);
      }
      as.mov(a64::x6, test_case.offset);
      as.add(a64::x5, base, a64::x6);
      if (indexed) {
        as.add(
            a64::x5,
            a64::x5,
            a64::x(test_case.index.loc),
            a64::lsl(test_case.multiplier));
      }
      as.mov(a64::x6, UINT64_MAX);
      as.str(a64::x6, a64::ptr(a64::x5));

      Function function;
      BasicBlock* block = function.allocateBasicBlock();
      Instruction* store = block->allocateInstr(
          Opcode::kStore,
          nullptr,
          OutInd{
              test_case.base,
              test_case.index,
              1u << test_case.multiplier,
              test_case.offset,
              type});
      if (test_case.immediate) {
        store->allocateImmediateInput(stored_value);
      } else {
        store->allocatePhyRegisterInput(test_case.value)->setDataType(type);
      }
      autogen::AutoTranslator::getInstance().translateInstr(&env, store);
      as.str(
          test_case.immediate ? a64::xzr : a64::x(test_case.value.loc),
          a64::ptr(a64::x2, 16));
      Instruction* load = block->allocateInstr(
          Opcode::kLoad,
          nullptr,
          OutPhyReg{X0, type},
          Ind{test_case.base,
              test_case.index,
              1u << test_case.multiplier,
              test_case.offset,
              type});
      autogen::AutoTranslator::getInstance().translateInstr(&env, load);
      as.str(a64::x0, a64::ptr(a64::x2, 24));
      as.str(base, a64::ptr(a64::x2));
      as.str(
          indexed ? a64::x(test_case.index.loc) : a64::xzr,
          a64::ptr(a64::x2, 8));
      as.ldr(a64::x0, a64::ptr(a64::x5));
      as.ret(a64::x30);
      ASSERT_EQ(as.finalize(), kErrorOk);
      JitRuntime runtime;
      uint64_t (*fn)(void*, uint64_t*) = nullptr;
      ASSERT_EQ(runtime.add(&fn, &code), kErrorOk);
      const unsigned bits = 8u << byteShift(type);
      const uint64_t mask = UINT64_MAX >> (64 - bits);
      uint64_t observed[4] = {};
      EXPECT_EQ(
          fn(memory, observed), (UINT64_MAX & ~mask) | (stored_value & mask));
      EXPECT_EQ(observed[0], reinterpret_cast<uint64_t>(memory + 4096));
      EXPECT_EQ(observed[1], indexed ? 5 : 0);
      EXPECT_EQ(observed[2], test_case.immediate ? 0 : stored_value);
      EXPECT_EQ(observed[3], stored_value & mask);
      ASSERT_EQ(runtime.release(fn), kErrorOk);
    }
  }
}
#endif

TEST_F(CodegenTest, DecodeX86InitialExecTls) {
  // push %rbp; mov %rsp,%rbp
  // mov %fs:0xfffffffffffffff0,%rax
  // pop %rbp; ret
  const uint8_t direct[] = {
      0x55,
      0x48,
      0x89,
      0xe5,
      0x64,
      0x48,
      0x8b,
      0x04,
      0x25,
      0xf0,
      0xff,
      0xff,
      0xff,
      0x5d,
      0xc3};
  EXPECT_EQ(decodeInitialExecOffset(direct), -0x10);

  // The thread pointer loaded first, from an aps_ads_vm Python 3.14 build:
  // push %rbp; mov %rsp,%rbp
  // mov %fs:0x0,%rax
  // mov -0x31ff0(%rax),%rax
  // pop %rbp; ret
  const uint8_t via_thread_ptr[] = {
      0x55, 0x48, 0x89, 0xe5, 0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00,
      0x00, 0x00, 0x48, 0x8b, 0x80, 0x10, 0xe0, 0xfc, 0xff, 0x5d, 0xc3};
  EXPECT_EQ(decodeInitialExecOffset(via_thread_ptr), -0x31ff0);

  // A second load after a nonzero %fs displacement isn't a known shape.
  const uint8_t nonzero_then_load[] = {
      0x55, 0x48, 0x89, 0xe5, 0x64, 0x48, 0x8b, 0x04, 0x25, 0xf0, 0xff,
      0xff, 0xff, 0x48, 0x8b, 0x80, 0x10, 0xe0, 0xfc, 0xff, 0x5d, 0xc3};
  EXPECT_EQ(decodeInitialExecOffset(nonzero_then_load), -1);

  // The %fs load must be followed by the epilogue or the second load.
  const uint8_t no_epilogue[] = {
      0x55,
      0x48,
      0x89,
      0xe5,
      0x64,
      0x48,
      0x8b,
      0x04,
      0x25,
      0x00,
      0x00,
      0x00,
      0x00,
      0x90,
      0x90,
      0x90};
  EXPECT_EQ(decodeInitialExecOffset(no_epilogue), -1);

  // No frame-pointer prologue.
  const uint8_t no_prologue[] = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0xf0, 0xff, 0xff, 0xff, 0xc3};
  EXPECT_EQ(decodeInitialExecOffset(no_prologue), -1);
}

TEST_F(CodegenTest, DecodeFramelessInitialExecTls) {
  // mrs x8, tpidr_el0
  // add x8, x8, #0x0, lsl #12
  // add x8, x8, #0x980
  // ldr x0, [x8]
  // ret
  const uint32_t getter[] = {
      0xd53bd048, 0x91400108, 0x91260108, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(getter), 0x980);

  // No adds: offset is zero.
  const uint32_t bare[] = {0xd53bd048, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(bare), 0);

  // Only the shifted add.
  const uint32_t hi_only[] = {0xd53bd048, 0x91400508, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(hi_only), 0x1000);

  // Only the unshifted add.
  const uint32_t lo_only[] = {0xd53bd048, 0x91260108, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(lo_only), 0x980);

  // The unshifted add must come after the shifted one.
  const uint32_t reversed[] = {
      0xd53bd048, 0x91260108, 0x91400108, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(reversed), -1);

  // At most one add of each kind.
  const uint32_t two_lo[] = {
      0xd53bd048, 0x91260108, 0x91260108, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(two_lo), -1);

  // ldr before the adds.
  const uint32_t ldr_first[] = {0xd53bd048, 0xf9400100, 0x91260108, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(ldr_first), -1);

  // First word isn't an mrs of tpidr_el0.
  const uint32_t prologue[] = {
      0xa9bf7bfd, 0x910003fd, 0xd53bd048, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(prologue), -1);

  // Middle add targets a different register.
  const uint32_t clobber[] = {0xd53bd048, 0x91260129, 0xf9400100, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(clobber), -1);

  // ldr with a nonzero offset isn't the expected shape.
  const uint32_t ldr_off[] = {0xd53bd048, 0xf9400500, 0xd65f03c0};
  EXPECT_EQ(decodeFramelessInitialExecOffset(ldr_off), -1);

  // Missing trailing ret: could be a prefix of a longer function.
  const uint32_t no_ret[] = {0xd53bd048, 0xf9400100, 0xd503201f};
  EXPECT_EQ(decodeFramelessInitialExecOffset(no_ret), -1);
}

} // namespace cinderx::jit::codegen
