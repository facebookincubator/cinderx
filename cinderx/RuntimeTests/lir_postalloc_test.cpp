// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit;
using namespace jit::codegen;

namespace jit::lir {
class LIRPostAllocRewriteTest : public RuntimeTest {};

TEST_F(LIRPostAllocRewriteTest, TestInsertBranchForSuccessorsInCondBranch) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
       CondBranch {}:Object, BB%1, BB%2
BB %1 - preds: %0 - succs: %3 %4
       CondBranch {}:Object, BB%3, BB%4
BB %2 - preds: %0 - succs: %3 %4
       CondBranch {}:Object, BB%3, BB%4
BB %3 - preds: %1 %2
       {} = Move {}:Object
BB %4 - preds: %1 %2
       {} = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   Test {}:Object, {}:Object
                   BranchNZ BB%1

BB %2 - preds: %0 - succs: %3 %4
                   Test {}:Object, {}:Object
                   BranchNZ BB%3
                   Branch BB%4

BB %1 - preds: %0 - succs: %3 %4
                   Test {}:Object, {}:Object
                   BranchZ BB%4

BB %3 - preds: %1 %2
{:>9}:Object = Move {}:Object

BB %4 - preds: %1 %2
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

TEST_F(
    LIRPostAllocRewriteTest,
    TestInsertBranchForSuccessorsInCondBranchDifferentSection) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2 - section: .text
       CondBranch {}:Object, BB%1, BB%2
BB %1 - preds: %0 - section: .coldtext
       {}:Object = Move {}:Object
BB %2 - preds: %0
       {}:Object = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   Test {}:Object, {}:Object
                   BranchZ BB%2
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

BB %2 - preds: %0
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

TEST_F(LIRPostAllocRewriteTest, TestInsertBranchInDifferentSection) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 - section: .text
{:>9}:Object = Move {}:Object
BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{7, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1
{:>9}:Object = Move {}:Object
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{7, 64});
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

#if defined(CINDER_AARCH64)
// Helper to collect instructions from a block into a vector for easy indexing.
static std::vector<Instruction*> collectInstrs(BasicBlock& bb) {
  std::vector<Instruction*> result;
  for (auto& instr : bb.instructions()) {
    result.push_back(instr.get());
  }
  return result;
}

// kAdd with one register input and one stack input should insert a Move from
// stack to GP scratch register before the Add, then rewrite the Add's stack
// input to use the scratch register.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteAddWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Add X0:64bit, [X29(-16)]:64bit
  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg{X0, DataType::k64bit},
      PhyReg{X0, DataType::k64bit},
      Stk{PhyLocation(-16, 64), DataType::k64bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Move (stack→scratch), Add (reg, scratch)
  ASSERT_EQ(instrs.size(), 2);

  // First instruction: Move from stack to scratch
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);
  EXPECT_TRUE(instrs[0]->getInput(0)->isStack());

  // Second instruction: Add with scratch register input
  EXPECT_TRUE(instrs[1]->isAdd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(instrs[1]->getInput(1)->getPhyRegister(), arch::reg_scratch_0_loc);
}

// kInc with a stack input should produce:
//   Move stack→scratch, Inc scratch, Move scratch→stack
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteIncWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Inc [X29(-24)]:Object
  bb->allocateInstr(
      Instruction::kInc,
      nullptr,
      OutStk{PhyLocation(-24, 64), DataType::kObject},
      Stk{PhyLocation(-24, 64), DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Move (stack→scratch), Inc (scratch), Move (scratch→stack)
  ASSERT_EQ(instrs.size(), 3);

  // Move from stack to scratch
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);

  // Inc on scratch
  EXPECT_TRUE(instrs[1]->isInc());
  EXPECT_TRUE(instrs[1]->getInput(0)->isReg());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), arch::reg_scratch_0_loc);

  // Move from scratch back to stack
  EXPECT_TRUE(instrs[2]->isMove());
  EXPECT_TRUE(instrs[2]->output()->isStack());
  EXPECT_TRUE(instrs[2]->getInput(0)->isReg());
  EXPECT_EQ(instrs[2]->getInput(0)->getPhyRegister(), arch::reg_scratch_0_loc);
}

// kFadd with a FP stack input should use FP scratch register (D16), not GP
// scratch (X13).
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteFaddWithFPStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Fadd D0:Double, [X29(-32)]:Double
  bb->allocateInstr(
      Instruction::kFadd,
      nullptr,
      OutPhyReg{D0, DataType::kDouble},
      PhyReg{D0, DataType::kDouble},
      Stk{PhyLocation(-32, 64), DataType::kDouble});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Move from stack to FP scratch (D16)
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_fp_scratch_0_loc);
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::kDouble);

  // Fadd with FP scratch register input
  EXPECT_TRUE(instrs[1]->isFadd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(
      instrs[1]->getInput(1)->getPhyRegister(), arch::reg_fp_scratch_0_loc);
}

// kLessThanSigned with a k8bit stack input should widen the load to k32bit to
// preserve the sign-extended value stored by rewriteSignedSubWordOps.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteSignedCmpWidensSubWordToK32) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // LessThanSigned X0:8bit, [X29(-8)]:8bit
  bb->allocateInstr(
      Instruction::kLessThanSigned,
      nullptr,
      OutPhyReg{X0, DataType::k8bit},
      PhyReg{X0, DataType::k8bit},
      Stk{PhyLocation(-8, 8), DataType::k8bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Move from stack to scratch — should be widened to k32bit
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::k32bit);
  EXPECT_EQ(instrs[0]->getInput(0)->dataType(), DataType::k32bit);

  // The comparison input should also be k32bit
  EXPECT_TRUE(instrs[1]->isLessThanSigned());
  EXPECT_EQ(instrs[1]->getInput(1)->dataType(), DataType::k32bit);
}
#endif // CINDER_AARCH64

} // namespace jit::lir
