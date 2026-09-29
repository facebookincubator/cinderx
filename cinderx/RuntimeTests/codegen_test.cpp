// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
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

} // namespace cinderx::jit::codegen
