// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace asmjit;
using namespace jit;
using namespace jit::codegen;

class RegisterPreserverTest : public RuntimeTest {
 public:
  arch::Reg gp(int32_t id) {
#if defined(CINDER_X86_64)
    return x86::gpq(id);
#elif defined(CINDER_AARCH64)
    return a64::x(id);
#else
    return BaseReg();
#endif
  }
};

TEST_F(RegisterPreserverTest, TestPreserveRestore) {
  auto codeAllocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());

  CodeHolder code;
  code.init(codeAllocator->asmJitEnvironment());

  arch::Builder as(&code);
  std::array<arch::Reg, 5> regs = {gp(0), gp(1), gp(2), gp(3), gp(4)};
  std::vector<std::pair<const arch::Reg&, const arch::Reg&>> pairs;
  pairs.reserve(regs.size());
  for (size_t idx = 0; idx < regs.size(); ++idx) {
    pairs.emplace_back(regs[idx], regs[idx]);
  }

  auto preserveLabel = as.newLabel();
  auto restoreLabel = as.newLabel();
  auto doneLabel = as.newLabel();

  auto preserver = RegisterPreserver(&as, pairs);
  as.bind(preserveLabel);
  preserver.preserve();
  as.bind(restoreLabel);
  preserver.restore();
  as.bind(doneLabel);
  as.finalize();

  AllocateResult result = codeAllocator->addCode(&code);
  EXPECT_EQ(result.error, asmjit::kErrorOk);

  uint64_t preserveOffset = code.labelOffset(preserveLabel);
  uint64_t restoreOffset = code.labelOffset(restoreLabel);
  uint64_t doneOffset = code.labelOffset(doneLabel);

  uint64_t preserveSize = restoreOffset - preserveOffset;
  uint64_t restoreSize = doneOffset - restoreOffset;

#if defined(CINDER_X86_64)
  // When there are an odd number of registers being preserved on x86-64, we
  // align the stack by a `push rax`/`add rsp, 8` pair. That means there is one
  // byte for the preserve and 4 bytes for the restore. To check that they are
  // equal, we subtract 3 here to ensure we are doing mirroring operations.
  restoreSize -= 3;
#endif

  EXPECT_EQ(preserveSize, restoreSize);
}

TEST_F(RegisterPreserverTest, TestRemap) {
  auto codeAllocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());

  CodeHolder code;
  code.init(codeAllocator->asmJitEnvironment());

  arch::Builder as(&code);
  std::array<arch::Reg, 5> regs = {gp(0), gp(1), gp(2), gp(3), gp(4)};

  std::vector<std::pair<const arch::Reg&, const arch::Reg&>> pairs;
  pairs.reserve(regs.size() - 1);
  for (size_t idx = 0; idx < regs.size() - 1; ++idx) {
    pairs.emplace_back(regs[idx], regs[idx + 1]);
  }

  auto remapLabel = as.newLabel();
  auto doneLabel = as.newLabel();

  auto preserver = RegisterPreserver(&as, pairs);
  as.bind(remapLabel);
  preserver.remap();
  as.bind(doneLabel);
  as.finalize();

  AllocateResult result = codeAllocator->addCode(&code);
  EXPECT_EQ(result.error, asmjit::kErrorOk);

  uint64_t remapOffset = code.labelOffset(remapLabel);
  uint64_t doneOffset = code.labelOffset(doneLabel);

  // Assert that we have done an equal-sized operation for each register pair.
  EXPECT_EQ((doneOffset - remapOffset) % (regs.size() - 1), 0);
}
