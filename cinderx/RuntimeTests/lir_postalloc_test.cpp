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
#include "cinderx/Jit/lir/printer.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <algorithm>
#include <iostream>

using namespace cinderx::jit;
using namespace cinderx::jit::codegen;

namespace cinderx::jit::lir {
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
#if defined(CINDER_AARCH64)
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   CmpBranchNonZero {}:Object, BB%1

BB %2 - preds: %0 - succs: %3 %4
                   CmpBranchNonZero {}:Object, BB%3
                   Branch BB%4

BB %1 - preds: %0 - succs: %3 %4
                   CmpBranchZero {}:Object, BB%4

BB %3 - preds: %1 %2
{:>9}:Object = Move {}:Object

BB %4 - preds: %1 %2
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});
#else
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
#endif
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
#if defined(CINDER_AARCH64)
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   CmpBranchZero {}:Object, BB%2
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

BB %2 - preds: %0
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});
#else
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
#endif
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

// Helper to collect instructions from a block into a vector for easy indexing.
static std::vector<Instruction*> collectInstrs(BasicBlock& bb) {
  std::vector<Instruction*> result;
  for (auto& instr : bb.instructions()) {
    result.push_back(instr.get());
  }
  return result;
}

// optimizeMoveSequence forwards a spill slot back to the register it was
// copied from.  A widening move in between only writes its own output, so it
// must not throw away the rest of what the pass knows.
TEST_F(LIRPostAllocRewriteTest, MoveSequenceLooksPastWideningMoves) {
  constexpr PhyLocation kSpilled = ARGUMENT_REGS[0];
  constexpr PhyLocation kWidenOut = ARGUMENT_REGS[1];
  constexpr PhyLocation kReloaded = ARGUMENT_REGS[2];
  constexpr PhyLocation kWidenIn = ARGUMENT_REGS[3];
  constexpr PhyLocation kSlot{-16, 64};

  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutStk{kSlot, DataType::k64bit},
      PhyReg{kSpilled, DataType::k64bit});
  bb->allocateInstr(
      Opcode::kZext,
      nullptr,
      OutPhyReg{kWidenOut, DataType::k64bit},
      PhyReg{kWidenIn, DataType::k32bit});
  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{kReloaded, DataType::k64bit},
      Stk{kSlot, DataType::k64bit});

  Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);

  // The reload reads the register the spill came from rather than the slot.
  EXPECT_TRUE(instrs[2]->isMove());
  EXPECT_TRUE(instrs[2]->getInput(0)->isReg());
  EXPECT_EQ(instrs[2]->getInput(0)->getPhyRegister(), kSpilled);
}

// kVectorCall invokes a vectorcallfunc pointer directly, so unlike
// kVectorCallTstate there is no thread state to pass: the callable is the first
// C argument and everything else shifts down one register.
TEST_F(LIRPostAllocRewriteTest, VectorCallPassesCallableAsFirstArgument) {
  constexpr uint64_t kVectorcallPtr = 123456789;
  // Homed somewhere other than the register it has to end up in, so the move
  // into place is observable.
  constexpr PhyLocation kCallableHome = ARGUMENT_REGS[1];

  Function func;
  auto* bb = func.allocateBasicBlock();

  // #0 vectorcall pointer, #1 flags, #2 callable, #3-4 args, #5 kwnames.
  bb->allocateInstr(
      Opcode::kVectorCall,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      Imm{kVectorcallPtr, DataType::k64bit},
      Imm{0, DataType::k64bit},
      PhyReg{kCallableHome, DataType::kObject},
      Imm{0xaaaa, DataType::kObject},
      Imm{0xbbbb, DataType::kObject},
      Imm{0, DataType::k64bit});

  Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_FALSE(instrs.empty());

  // The callable moves into the first argument register, ahead of the argument
  // buffer setup that overwrites the register it came from.
  ASSERT_TRUE(instrs[0]->isMove());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), ARGUMENT_REGS[0]);
  ASSERT_TRUE(instrs[0]->getInput(0)->isReg());
  EXPECT_EQ(instrs[0]->getInput(0)->getPhyRegister(), kCallableHome);

  // nargsf is a plain count of the Python arguments.  kVectorCall does not
  // advertise PY_VECTORCALL_ARGUMENTS_OFFSET, so the flag bit must be clear.
  auto nargsf = std::ranges::find_if(instrs, [&](const Instruction* instr) {
    return instr->isMove() && instr->output()->isReg() &&
        instr->output()->getPhyRegister() == ARGUMENT_REGS[2] &&
        instr->getInput(0)->isImm() && instr->getInput(0)->getConstant() == 2;
  });
  EXPECT_NE(nargsf, instrs.end()) << "no nargsf setup for 2";
  for (const Instruction* instr : instrs) {
    for (size_t i = 0; i < instr->getNumInputs(); ++i) {
      const Operand* in = instr->getInput(i);
      if (in->isImm()) {
        EXPECT_EQ(in->getConstant() & PY_VECTORCALL_ARGUMENTS_OFFSET, 0u)
            << "kVectorCall must not set PY_VECTORCALL_ARGUMENTS_OFFSET";
      }
    }
  }

  // The pseudo-opcode is gone: what is left is a plain indirect call of the
  // vectorcall pointer, with no thread state operand.
  const Instruction* call = instrs.back();
  ASSERT_TRUE(call->isCall());
  ASSERT_EQ(call->getNumInputs(), 1);
  ASSERT_TRUE(call->getInput(0)->isImm());
  EXPECT_EQ(call->getInput(0)->getConstant(), kVectorcallPtr);

  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

// With no arguments and no PY_VECTORCALL_ARGUMENTS_OFFSET scratch slot a 0-arg
// kVectorCall has nothing to marshal, so it should reserve nothing beyond the
// callee's home space (kShadowSpaceSize, which is 0 off Windows x64).  The args
// pointer is a don't-care that the callee never reads, so nothing should be
// materialized into its register either -- not a stack address and not a zero.
TEST_F(LIRPostAllocRewriteTest, ZeroArgVectorCallReservesOnlyShadowSpace) {
  constexpr uint64_t kVectorcallPtr = 123456789;

  Function func;
  auto* bb = func.allocateBasicBlock();

  // #0 vectorcall pointer, #1 flags, #2 callable, #3 kwnames.  No arguments.
  bb->allocateInstr(
      Opcode::kVectorCall,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      Imm{kVectorcallPtr, DataType::k64bit},
      Imm{0, DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::kObject},
      Imm{0, DataType::k64bit});

  Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  // No args buffer, but on Windows x64 the callee's home space must still be
  // reserved -- the callee may write [SP+0, +32) regardless of how few
  // arguments it was passed.
  EXPECT_EQ(env.max_arg_buffer_size, kShadowSpaceSize)
      << "a 0-arg vectorcall should reserve exactly the callee home space";

  // kVectorCall puts the callable in ARGUMENT_REGS[0], so the args pointer is
  // ARGUMENT_REGS[1].  Nothing may write it.
  constexpr PhyLocation kArgsReg = ARGUMENT_REGS[1];

  auto instrs = collectInstrs(*bb);
  for (const Instruction* instr : instrs) {
    // No stack address is computed for the args array.
    EXPECT_FALSE(instr->isLea())
        << "0-arg vectorcall should not compute a stack args pointer";
    const Operand* out = instr->output();
    if (out->isReg()) {
      EXPECT_NE(out->getPhyRegister(), kArgsReg)
          << "0-arg vectorcall should leave the args register untouched, "
             "found: "
          << *instr;
    }
    for (size_t i = 0; i < instr->getNumInputs(); ++i) {
      const Operand* in = instr->getInput(i);
      if (in->isImm()) {
        EXPECT_EQ(in->getConstant() & PY_VECTORCALL_ARGUMENTS_OFFSET, 0u);
      }
    }
  }

  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

// The args array must never overlap the callee's home space.  On Windows x64
// the callee owns [SP+0, SP+32) and may spill its register arguments there at
// any point, so the buffer has to start at kShadowSpaceSize and the call has to
// reserve that space on top of the buffer itself.  kShadowSpaceSize is 0 off
// Windows, where this pins the buffer at SP+0 as before.
TEST_F(LIRPostAllocRewriteTest, VectorCallArgsAvoidCalleeHomeSpace) {
  constexpr uint64_t kVectorcallPtr = 123456789;

  Function func;
  auto* bb = func.allocateBasicBlock();

  // #0 vectorcall pointer, #1 flags, #2 callable, #3-4 args, #5 kwnames.
  bb->allocateInstr(
      Opcode::kVectorCall,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      Imm{kVectorcallPtr, DataType::k64bit},
      Imm{0, DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::kObject},
      Imm{0xaaaa, DataType::kObject},
      Imm{0xbbbb, DataType::kObject},
      Imm{0, DataType::k64bit});

  Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  // The lea that materializes the args pointer must start at the shadow space,
  // not below it.
  auto instrs = collectInstrs(*bb);
  auto lea = std::ranges::find_if(
      instrs, [](const Instruction* instr) { return instr->isLea(); });
  ASSERT_NE(lea, instrs.end()) << "expected an args-array lea";
  const Operand* addr = (*lea)->getInput(0);
  ASSERT_TRUE(addr->isInd());
  EXPECT_EQ(addr->getMemoryIndirect()->getOffset(), kShadowSpaceSize)
      << "args array must start above the callee's home space";

  // ...and the reservation must cover the home space plus the two arguments,
  // since max_arg_buffer_size is what sizes the bottom of the frame.
  EXPECT_GE(env.max_arg_buffer_size, kShadowSpaceSize + 2 * kPointerSize);

  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

#if defined(CINDER_AARCH64)
// kAdd with one register input and one stack input should insert a Load from
// stack to GP scratch register before the Add, then rewrite the Add's stack
// input to use the scratch register.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteAddWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Add X0:64bit, [X29(-16)]:64bit
  bb->allocateInstr(
      Opcode::kAdd,
      nullptr,
      OutPhyReg{X0, DataType::k64bit},
      PhyReg{X0, DataType::k64bit},
      Stk{PhyLocation(-16, 64), DataType::k64bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Load (stack→scratch), Add (reg, scratch)
  ASSERT_EQ(instrs.size(), 2);

  // First instruction: Load from stack to scratch
  EXPECT_TRUE(instrs[0]->isLoad());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);
  EXPECT_TRUE(instrs[0]->getInput(0)->isStack());

  // Second instruction: Add with scratch register input
  EXPECT_TRUE(instrs[1]->isAdd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(instrs[1]->getInput(1)->getPhyRegister(), arch::reg_scratch_0_loc);
}

// kInc with a stack input should produce:
//   Load stack→scratch, Inc scratch, Store scratch→stack
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteIncWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Inc [X29(-24)]:Object
  bb->allocateInstr(
      Opcode::kInc,
      nullptr,
      OutStk{PhyLocation(-24, 64), DataType::kObject},
      Stk{PhyLocation(-24, 64), DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Load (stack→scratch), Inc (scratch), Store (scratch→stack)
  ASSERT_EQ(instrs.size(), 3);

  // Load from stack to scratch
  EXPECT_TRUE(instrs[0]->isLoad());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);

  // Inc on scratch
  EXPECT_TRUE(instrs[1]->isInc());
  EXPECT_TRUE(instrs[1]->getInput(0)->isReg());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), arch::reg_scratch_0_loc);

  // Store from scratch back to stack
  EXPECT_TRUE(instrs[2]->isStore());
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
      Opcode::kFadd,
      nullptr,
      OutPhyReg{D0, DataType::kDouble},
      PhyReg{D0, DataType::kDouble},
      Stk{PhyLocation(-32, 64), DataType::kDouble});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Load from stack to FP scratch (D16)
  EXPECT_TRUE(instrs[0]->isLoad());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_fp_scratch_0_loc);
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::kDouble);

  // Fadd with FP scratch register input
  EXPECT_TRUE(instrs[1]->isFadd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(
      instrs[1]->getInput(1)->getPhyRegister(), arch::reg_fp_scratch_0_loc);
}

// A signed compare with a k8bit stack input should widen the load to k32bit to
// preserve the sign-extended value stored by rewriteSignedSubWordOps.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteSignedCmpWidensSubWordToK32) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // LessThanSigned X0:8bit, [X29(-8)]:8bit
  bb->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kSignedLT,
      OutPhyReg{X0, DataType::k8bit},
      PhyReg{X0, DataType::k8bit},
      Stk{PhyLocation(-8, 8), DataType::k8bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Load from stack to scratch — should be widened to k32bit
  EXPECT_TRUE(instrs[0]->isLoad());
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::k32bit);
  EXPECT_EQ(instrs[0]->getInput(0)->dataType(), DataType::k32bit);

  // The comparison input should also be k32bit
  EXPECT_TRUE(instrs[1]->isCompare());
  EXPECT_EQ(instrs[1]->getInput(1)->dataType(), DataType::k32bit);
}

TEST_F(LIRPostAllocRewriteTest, VectorCallArgsUseStorePairForRegisterPairs) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0
{:>9}:Object = VectorCallTstate 123456789, 0, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, 0
)",
      X0,
      X1,
      X2,
      X3,
      X4,
      X5,
      X6);

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(parsed_func.get(), &env);
  rewrite.run();

  size_t store_pairs = 0;
  for (auto& instr : parsed_func->basicBlocks().front()->instructions()) {
    store_pairs += instr->isStorePair() ? 1 : 0;
  }

  EXPECT_EQ(store_pairs, 2);
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

TEST_F(LIRPostAllocRewriteTest, RegularCallArgsUseStorePairForRegisterPairs) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0
                   Call 123456789, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object, {:>9}:Object
)",
      X0,
      X1,
      X2,
      X3,
      X4,
      X5,
      X6,
      X7,
      X8,
      X9);

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(parsed_func.get(), &env);
  rewrite.run();

  const Instruction* store_pair = nullptr;
  for (auto& instr : parsed_func->basicBlocks().front()->instructions()) {
    if (instr->isStorePair()) {
      ASSERT_EQ(store_pair, nullptr);
      store_pair = instr.get();
    }
  }

  ASSERT_NE(store_pair, nullptr);
  EXPECT_EQ(
      store_pair->getInput(1)->getPhyRegister(), arch::reg_stack_pointer_loc);
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}
TEST_F(LIRPostAllocRewriteTest, AdjacentFrameSlotStoresBecomeStorePair) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutStk{PhyLocation(-24, 64), DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutStk{PhyLocation(-16, 64), DataType::k64bit},
      PhyReg{X1, DataType::k64bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();
  runPostRegAllocPeephole(&func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 1);
  ASSERT_TRUE(instrs[0]->isStorePair());
  // The lower address comes first, so the slot at -24 supplies the offset.
  EXPECT_EQ(static_cast<int32_t>(instrs[0]->getInput(0)->getConstant()), -24);
  EXPECT_EQ(
      instrs[0]->getInput(1)->getPhyRegister(), arch::reg_frame_pointer_loc);
  EXPECT_EQ(instrs[0]->getInput(2)->getPhyRegister(), X0);
  EXPECT_EQ(instrs[0]->getInput(3)->getPhyRegister(), X1);
}

TEST_F(LIRPostAllocRewriteTest, DescendingFrameSlotLoadsBecomeLoadPair) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Written high address first, so the pair has to swap the register order.
  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{X0, DataType::k64bit},
      Stk{PhyLocation(-16, 64), DataType::k64bit});
  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      Stk{PhyLocation(-24, 64), DataType::k64bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();
  runPostRegAllocPeephole(&func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 1);
  ASSERT_TRUE(instrs[0]->isLoadPair());
  EXPECT_EQ(static_cast<int32_t>(instrs[0]->getInput(0)->getConstant()), -24);
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), X1);
  EXPECT_EQ(instrs[0]->getInput(2)->getPhyRegister(), X0);
}

// ldp with a destination that is also its base is unpredictable, and the
// unmerged form would have fed the first load's result into the second's
// address, so this pair has to be left alone.
TEST_F(LIRPostAllocRewriteTest, LoadPairSkippedWhenDestinationIsBase) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{X2, DataType::k64bit},
      Ind(X2, static_cast<int32_t>(0)));
  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{X3, DataType::k64bit},
      Ind(X2, static_cast<int32_t>(8)));

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();
  runPostRegAllocPeephole(&func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);
  EXPECT_FALSE(instrs[0]->isLoadPair());
  EXPECT_FALSE(instrs[1]->isLoadPair());
}

#endif // CINDER_AARCH64

} // namespace cinderx::jit::lir
