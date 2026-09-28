// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/lir/verify.h"

#include <sstream>

namespace cinderx::jit::lir {

TEST(LIRVerifyTest, TestImmediateFallthroughOK) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  first->addSuccessor(second);

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors));
}

TEST(LIRVerifyTest, TestNonImmediateFallthroughDisallowed) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  function.allocateBasicBlock();
  BasicBlock* third = function.allocateBasicBlock();
  first->addSuccessor(third);

  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(),
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "2.\n");
}

TEST(LIRVerifyTest, TestSingleSuccessorOK) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* third = function.allocateBasicBlock();
  first->addSuccessor(second);
  second->addSuccessor(third);

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors));
}

TEST(LIRVerifyTest, TestAllSuccessorsChecked) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* third = function.allocateBasicBlock();
  first->addSuccessor(second);
  first->addSuccessor(third);
  second->addSuccessor(third);

  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(),
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "2.\n");
}

TEST(LIRVerifyTest, TestExplicitBranchOK) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  function.allocateBasicBlock();
  BasicBlock* third = function.allocateBasicBlock();
  first->allocateInstr(Opcode::kBranch, nullptr, Lbl{third});
  first->addSuccessor(third);

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors));
}

TEST(LIRVerifyTest, TestExplicitConditionalBranchOK) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* third = function.allocateBasicBlock();
  first->allocateInstr(
      Opcode::kBranchCC, nullptr, Condition::kZero, Lbl{third});
  first->addSuccessor(second);
  first->addSuccessor(third);

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors));
}

TEST(LIRVerifyTest, TestFallthroughToBlockInDifferentSectionDisallowed) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  second->setSection(codegen::CodeSection::kCold);
  first->addSuccessor(second);

  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(),
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "1.\n");
}

} // namespace cinderx::jit::lir
