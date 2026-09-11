// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/linear_scan.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/spill_alloc.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_query.h"

#include <math.h>

#include <algorithm>
#include <memory>
#include <ostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cinderx {

using namespace asmjit;
using namespace cinderx::jit;
using namespace cinderx::jit::lir;

TEST(LIRTypeTest, DataTypeByteShift) {
  EXPECT_EQ(byteShift(DataType::k8bit), 0);
  EXPECT_EQ(byteShift(DataType::k16bit), 1);
  EXPECT_EQ(byteShift(DataType::k32bit), 2);
  EXPECT_EQ(byteShift(DataType::k64bit), 3);
  EXPECT_EQ(byteShift(DataType::kDouble), 3);
  EXPECT_EQ(byteShift(DataType::kObject), 3);
}

// Conditions drive both the encoding and the printed spelling now, so a
// transcription slip in the table would quietly mis-encode a comparison rather
// than fail to build.
TEST(LIRConditionTest, NegationAndSwapAreInvolutions) {
#define CHECK_NEGATE(NAME, ...)                                      \
  EXPECT_EQ(negate(negate(Condition::k##NAME)), Condition::k##NAME); \
  EXPECT_NE(negate(Condition::k##NAME), Condition::k##NAME);
  FOREACH_LIR_CONDITION(CHECK_NEGATE)
#undef CHECK_NEGATE

#define CHECK_SWAP(NAME, NEGATED, SWAPPED, ...)                              \
  if (Condition::k##SWAPPED != Condition::kInvalid) {                        \
    EXPECT_EQ(                                                               \
        swapOperands(swapOperands(Condition::k##NAME)), Condition::k##NAME); \
  }
  FOREACH_LIR_CONDITION(CHECK_SWAP)
#undef CHECK_SWAP
}

TEST(LIRConditionTest, MnemonicNamesKeepTheirMeaning) {
  // BranchCC and Compare still print under the per-condition names the opcodes
  // used to have.  "Above" and "below" are the unsigned comparisons and
  // "greater" and "less" the signed ones, which is the easy thing to get
  // backwards in a table this shape.
  EXPECT_EQ(branchCCName(Condition::kUnsignedGT), "BranchA");
  EXPECT_EQ(branchCCName(Condition::kUnsignedGE), "BranchAE");
  EXPECT_EQ(branchCCName(Condition::kUnsignedLT), "BranchB");
  EXPECT_EQ(branchCCName(Condition::kUnsignedLE), "BranchBE");
  EXPECT_EQ(branchCCName(Condition::kSignedGT), "BranchG");
  EXPECT_EQ(branchCCName(Condition::kSignedGE), "BranchGE");
  EXPECT_EQ(branchCCName(Condition::kSignedLT), "BranchL");
  EXPECT_EQ(branchCCName(Condition::kSignedLE), "BranchLE");
  EXPECT_EQ(branchCCName(Condition::kZero), "BranchZ");
  EXPECT_EQ(branchCCName(Condition::kNotZero), "BranchNZ");
  EXPECT_EQ(branchCCName(Condition::kEqual), "BranchE");
  EXPECT_EQ(branchCCName(Condition::kSign), "BranchS");

  EXPECT_EQ(compareName(Condition::kUnsignedGT), "GreaterThanUnsigned");
  EXPECT_EQ(compareName(Condition::kSignedGT), "GreaterThanSigned");
  EXPECT_EQ(compareName(Condition::kUnsignedLE), "LessThanEqualUnsigned");
  EXPECT_EQ(compareName(Condition::kSignedLE), "LessThanEqualSigned");
  EXPECT_EQ(compareName(Condition::kEqual), "Equal");

  // Negation and operand swapping mirror the relation the right way round.
  EXPECT_EQ(negate(Condition::kUnsignedGT), Condition::kUnsignedLE);
  EXPECT_EQ(negate(Condition::kSignedLT), Condition::kSignedGE);
  EXPECT_EQ(negate(Condition::kZero), Condition::kNotZero);
  EXPECT_EQ(swapOperands(Condition::kSignedGT), Condition::kSignedLT);
  EXPECT_EQ(swapOperands(Condition::kUnsignedLE), Condition::kUnsignedGE);
  EXPECT_EQ(swapOperands(Condition::kEqual), Condition::kEqual);

  // Only the comparisons are signed or unsigned; a flag test is neither.
  EXPECT_TRUE(isSignedCompare(Condition::kSignedGE));
  EXPECT_FALSE(isSignedCompare(Condition::kUnsignedGE));
  EXPECT_FALSE(isSignedCompare(Condition::kSign));
}

namespace {

size_t blockCount(
    const std::vector<BasicBlock*>& blocks,
    const BasicBlock* block) {
  return std::count(blocks.begin(), blocks.end(), block);
}

void expectEdgeCount(
    const BasicBlock* predecessor,
    const BasicBlock* successor,
    size_t expected) {
  EXPECT_EQ(blockCount(predecessor->successors(), successor), expected);
  EXPECT_EQ(blockCount(successor->predecessors(), predecessor), expected);
}

void expectIncomingEdge(
    const IncomingEdge& edge,
    BasicBlock* predecessor,
    BasicBlock* successor,
    size_t incoming_slot) {
  EXPECT_EQ(edge.predecessor(), predecessor);
  EXPECT_EQ(edge.successor(), successor);
  EXPECT_EQ(edge.incomingSlot(), incoming_slot);
}

lir::Operand*
addImmediatePhiInput(Instruction* phi, IncomingEdge edge, uint64_t value) {
  auto operand = std::make_unique<lir::Operand>(phi);
  lir::Operand* operand_ptr = operand.get();
  operand->setConstant(value);
  phi->addPhiInput(edge, std::move(operand));
  return operand_ptr;
}

Instruction* findPhiWithInputCount(Function* function, size_t input_count) {
  for (BasicBlock* block : function->basicBlocks()) {
    for (auto& instruction : block->instructions()) {
      if (instruction->isPhi() && instruction->numPhiInputs() == input_count) {
        return instruction.get();
      }
    }
  }
  return nullptr;
}

void expectPhiInputsFollowPredecessors(Instruction* phi) {
  ASSERT_NE(phi, nullptr);
  BasicBlock* block = phi->basicBlock();
  ASSERT_EQ(phi->numPhiInputs(), block->numPredecessors());
  for (size_t index = 0; index < phi->numPhiInputs(); ++index) {
    EXPECT_EQ(phi->phiPredecessor(index), block->predecessor(index));
    EXPECT_NE(phi->phiInput(index), nullptr);
  }
}

} // namespace

TEST(LIRBlockTest, AddSuccessorUpdatesEdges) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  BasicBlock* loop = function.allocateBasicBlock();

  first->addSuccessor(join);
  first->addSuccessor(join);
  second->addSuccessor(join);
  loop->addSuccessor(loop);

  expectEdgeCount(first, join, 2);
  expectEdgeCount(second, join, 1);
  expectEdgeCount(loop, loop, 1);
}

TEST(LIRBlockTest, AddSuccessorReturnsIncomingSlots) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* target = function.allocateBasicBlock();
  BasicBlock* loop = function.allocateBasicBlock();

  IncomingEdge first = source->addSuccessor(target);
  IncomingEdge second = other->addSuccessor(target);
  IncomingEdge duplicate = source->addSuccessor(target);
  IncomingEdge self = loop->addSuccessor(loop);

  expectIncomingEdge(first, source, target, 0);
  expectIncomingEdge(second, other, target, 1);
  expectIncomingEdge(duplicate, source, target, 2);
  expectIncomingEdge(self, loop, loop, 0);
  expectIncomingEdge(target->incomingEdge(0), source, target, 0);
  expectIncomingEdge(target->incomingEdge(1), other, target, 1);
  expectIncomingEdge(target->incomingEdge(2), source, target, 2);
  EXPECT_THROW(target->incomingEdge(3), std::runtime_error);
  expectIncomingEdge(source->outgoingEdge(0), source, target, 0);
  expectIncomingEdge(source->outgoingEdge(1), source, target, 2);
  expectIncomingEdge(other->outgoingEdge(0), other, target, 1);
  EXPECT_EQ(first.outgoingSlot(), 0);
  EXPECT_EQ(duplicate.outgoingSlot(), 1);
  EXPECT_EQ(second.outgoingSlot(), 0);
  EXPECT_THROW(source->outgoingEdge(2), std::runtime_error);
}

TEST(LIRBlockTest, PhiAccessorsUseIncomingSlots) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  IncomingEdge first_edge = first->addSuccessor(join);
  IncomingEdge second_edge = second->addSuccessor(join);

  Instruction* first_phi =
      join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  auto* first_value = addImmediatePhiInput(first_phi, first_edge, 10);
  auto* second_value = addImmediatePhiInput(first_phi, second_edge, 20);

  Instruction* second_phi =
      join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(second_phi, first_edge, 100);
  addImmediatePhiInput(second_phi, second_edge, 200);

  EXPECT_EQ(first_phi->numPhiInputs(), 2);
  EXPECT_EQ(first_phi->getNumInputs(), 2);
  EXPECT_EQ(first_phi->phiPredecessor(0), first);
  EXPECT_EQ(first_phi->phiPredecessor(1), second);
  EXPECT_EQ(first_phi->phiInput(0), first_value);
  EXPECT_EQ(first_phi->phiInput(1), second_value);

  const Instruction* const_phi = second_phi;
  EXPECT_EQ(const_phi->numPhiInputs(), 2);
  EXPECT_EQ(const_phi->phiPredecessor(1), second);
  EXPECT_EQ(const_phi->phiInput(0)->getConstant(), 100);
  EXPECT_EQ(const_phi->phiInput(1)->getConstant(), 200);

  EXPECT_DEATH(first_phi->phiInput(2), "Phi input index out of range");
}

TEST(LIRBlockTest, AddPhiInputUsesIncomingSlot) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  IncomingEdge first_edge = first->addSuccessor(join);

  Instruction* first_value =
      first->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{10});
  Instruction* second_value =
      second->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{20});
  auto moved_value = second_value->removeInput(0);
  auto* moved_value_ptr = moved_value.get();
  moved_value_ptr->setLastUse();

  Instruction* phi = join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  EXPECT_EQ(phi->getNumInputs(), 1);

  IncomingEdge second_edge = second->addSuccessor(join);
  EXPECT_EQ(phi->getNumInputs(), 2);
  phi->addPhiInput(second_edge, std::move(moved_value));

  EXPECT_EQ(phi->numPhiInputs(), 2);
  EXPECT_EQ(phi->phiInput(0), nullptr);
  EXPECT_EQ(phi->phiInput(1), moved_value_ptr);
  EXPECT_EQ(moved_value_ptr->instr(), phi);
  EXPECT_TRUE(moved_value_ptr->isLastUse());
  EXPECT_EQ(phi->phiPredecessor(0), first);
  EXPECT_EQ(phi->phiPredecessor(1), second);
  std::stringstream sparse_phi;
  sparse_phi << *phi;
  EXPECT_NE(sparse_phi.str().find("<unset>"), std::string::npos);

  phi->addPhiInput(first_edge, first_value);

  EXPECT_EQ(phi->getNumInputs(), join->numPredecessors());
  EXPECT_EQ(phi->phiInput(0)->getLinkedInstr(), first_value);
  EXPECT_EQ(phi->phiInput(1), moved_value_ptr);
  EXPECT_DEATH(
      phi->addPhiInput(first_edge, first_value), "Phi input already set");
}

TEST(LIRBlockTest, PhiInputsMustBeValues) {
  Function function;
  BasicBlock* predecessor = function.allocateBasicBlock();
  BasicBlock* block = function.allocateBasicBlock();
  predecessor->addSuccessor(block);
  Instruction* phi = block->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});

  EXPECT_DEATH(
      {
        auto label = std::make_unique<lir::Operand>(phi);
        label->setBasicBlock(predecessor);
        phi->setInput(0, std::move(label));
      },
      "Phi inputs must be values");

  Instruction* branch = block->allocateInstr(Opcode::kBranch, nullptr);
  EXPECT_EQ(branch->allocateLabelInput(block)->getBasicBlock(), block);
}

TEST(LIRFunctionTest, CopyFromPreservesPhiValuesAcrossEdgeReordering) {
  Function source;
  BasicBlock* entry = source.allocateBasicBlock();
  BasicBlock* first = source.allocateBasicBlock();
  BasicBlock* second = source.allocateBasicBlock();
  BasicBlock* join = source.allocateBasicBlock();
  BasicBlock* exit = source.allocateBasicBlock();

  entry->addSuccessor(first);
  entry->addSuccessor(second);
  IncomingEdge second_edge = second->addSuccessor(join);
  IncomingEdge first_edge = first->addSuccessor(join);
  IncomingEdge duplicate_first_edge = first->addSuccessor(join);
  join->addSuccessor(exit);

  Instruction* first_value =
      first->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{10});
  Instruction* duplicate_first_value =
      first->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{30});
  Instruction* second_value =
      second->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{20});
  Instruction* phi = join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  phi->addPhiInput(first_edge, first_value);
  phi->addPhiInput(duplicate_first_edge, duplicate_first_value);
  phi->addPhiInput(second_edge, second_value);

  Function destination;
  BasicBlock* previous = destination.allocateBasicBlock();
  BasicBlock* next = destination.allocateBasicBlock();
  previous->addSuccessor(next);
  destination.copyFrom(&source, previous, next, nullptr);

  Instruction* phi_copy = findPhiWithInputCount(&destination, 3);
  ASSERT_NE(phi_copy, nullptr);
  std::vector<uint64_t> copied_values;
  for (size_t index = 0; index < phi_copy->numPhiInputs(); ++index) {
    Instruction* value = phi_copy->phiInput(index)->getLinkedInstr();
    EXPECT_EQ(value->basicBlock(), phi_copy->phiPredecessor(index));
    copied_values.push_back(value->getInput(0)->getConstant());
  }
  const std::vector<uint64_t> expected{10, 30, 20};
  EXPECT_EQ(copied_values, expected);
}

TEST(LIRBlockTest, SetSuccessorUpdatesEdges) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* old_successor = function.allocateBasicBlock();
  BasicBlock* new_successor = function.allocateBasicBlock();

  IncomingEdge source_edge = source->addSuccessor(old_successor);
  IncomingEdge other_edge = other->addSuccessor(old_successor);
  Instruction* phi =
      old_successor->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, source_edge, 10);
  addImmediatePhiInput(phi, other_edge, 20);

  source->setSuccessor(0, source_edge.incomingSlot(), old_successor);
  EXPECT_EQ(phi->phiPredecessor(0), source);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 10);

  source->setSuccessor(0, source_edge.incomingSlot(), new_successor);

  expectEdgeCount(source, old_successor, 0);
  expectEdgeCount(source, new_successor, 1);
  expectEdgeCount(other, old_successor, 1);
  ASSERT_EQ(phi->numPhiInputs(), 1);
  EXPECT_EQ(phi->phiPredecessor(0), other);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 20);
}

TEST(LIRBlockTest, SetSuccessorUsesExactDuplicateEdge) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* old_successor = function.allocateBasicBlock();
  BasicBlock* new_successor = function.allocateBasicBlock();
  IncomingEdge first_edge = source->addSuccessor(old_successor);
  IncomingEdge other_edge = other->addSuccessor(old_successor);
  IncomingEdge duplicate_edge = source->addSuccessor(old_successor);
  Instruction* phi =
      old_successor->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, first_edge, 10);
  addImmediatePhiInput(phi, other_edge, 20);
  addImmediatePhiInput(phi, duplicate_edge, 30);

  source->setSuccessor(
      duplicate_edge.outgoingSlot(),
      duplicate_edge.incomingSlot(),
      new_successor);

  EXPECT_EQ(
      source->successors(),
      (std::vector<BasicBlock*>{old_successor, new_successor}));
  EXPECT_EQ(
      old_successor->predecessors(), (std::vector<BasicBlock*>{source, other}));
  EXPECT_EQ(new_successor->predecessors(), (std::vector<BasicBlock*>{source}));
  ASSERT_EQ(phi->numPhiInputs(), 2);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 10);
  EXPECT_EQ(phi->phiInput(1)->getConstant(), 20);
}

TEST(LIRBlockTest, PopSuccessorUpdatesEdges) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* first_successor = function.allocateBasicBlock();
  BasicBlock* last_successor = function.allocateBasicBlock();

  source->addSuccessor(first_successor);
  IncomingEdge source_edge = source->addSuccessor(last_successor);
  IncomingEdge other_edge = other->addSuccessor(last_successor);
  Instruction* phi =
      last_successor->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, source_edge, 10);
  addImmediatePhiInput(phi, other_edge, 20);

  source->popSuccessor();

  EXPECT_EQ(source->successors(), (std::vector<BasicBlock*>{first_successor}));
  expectEdgeCount(source, first_successor, 1);
  expectEdgeCount(source, last_successor, 0);
  expectEdgeCount(other, last_successor, 1);
  ASSERT_EQ(phi->numPhiInputs(), 1);
  EXPECT_EQ(phi->phiPredecessor(0), other);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 20);
}

TEST(LIRBlockTest, PopSuccessorRejectsEmptySuccessors) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();

  EXPECT_THROW(block->popSuccessor(), std::runtime_error);
}

TEST(LIRBlockTest, InsertBasicBlockBetweenUpdatesEdges) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* target = function.allocateBasicBlock();
  IncomingEdge source_edge = source->addSuccessor(target);
  IncomingEdge other_edge = other->addSuccessor(target);
  Instruction* phi = target->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, source_edge, 10);
  addImmediatePhiInput(phi, other_edge, 20);

  BasicBlock* inserted =
      source->insertBasicBlockBetween(target, source_edge.incomingSlot());

  expectEdgeCount(source, target, 0);
  expectEdgeCount(source, inserted, 1);
  expectEdgeCount(inserted, target, 1);
  expectEdgeCount(other, target, 1);
  EXPECT_EQ(phi->phiPredecessor(0), inserted);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 10);
  EXPECT_EQ(phi->phiPredecessor(1), other);
  EXPECT_EQ(phi->phiInput(1)->getConstant(), 20);
}

TEST(LIRBlockTest, InsertBasicBlockBetweenUsesExactDuplicateEdge) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* target = function.allocateBasicBlock();
  IncomingEdge first_edge = source->addSuccessor(target);
  IncomingEdge other_edge = other->addSuccessor(target);
  IncomingEdge duplicate_edge = source->addSuccessor(target);
  Instruction* phi = target->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, first_edge, 10);
  addImmediatePhiInput(phi, other_edge, 20);
  addImmediatePhiInput(phi, duplicate_edge, 30);

  BasicBlock* inserted =
      source->insertBasicBlockBetween(target, duplicate_edge.incomingSlot());

  EXPECT_EQ(source->successors(), (std::vector<BasicBlock*>{target, inserted}));
  EXPECT_EQ(
      target->predecessors(),
      (std::vector<BasicBlock*>{source, other, inserted}));
  EXPECT_EQ(inserted->successors(), (std::vector<BasicBlock*>{target}));
  EXPECT_EQ(phi->phiPredecessor(0), source);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 10);
  EXPECT_EQ(phi->phiPredecessor(1), other);
  EXPECT_EQ(phi->phiInput(1)->getConstant(), 20);
  EXPECT_EQ(phi->phiPredecessor(2), inserted);
  EXPECT_EQ(phi->phiInput(2)->getConstant(), 30);

  target->removePredecessor(0);
  EXPECT_EQ(source->successors(), (std::vector<BasicBlock*>{inserted}));
  EXPECT_EQ(
      target->predecessors(), (std::vector<BasicBlock*>{other, inserted}));
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 20);
  EXPECT_EQ(phi->phiInput(1)->getConstant(), 30);
}

TEST(LIRBlockTest, SplitBeforeUpdatesEdges) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* other = function.allocateBasicBlock();
  BasicBlock* first_target = function.allocateBasicBlock();
  BasicBlock* second_target = function.allocateBasicBlock();
  source->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{1});
  Instruction* split_point =
      source->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, lir::Imm{2});
  IncomingEdge source_first_edge = source->addSuccessor(first_target);
  IncomingEdge source_second_edge = source->addSuccessor(second_target);
  IncomingEdge other_first_edge = other->addSuccessor(first_target);
  Instruction* first_phi =
      first_target->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(first_phi, source_first_edge, 10);
  addImmediatePhiInput(first_phi, other_first_edge, 20);
  Instruction* second_phi =
      second_target->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(second_phi, source_second_edge, 30);

  BasicBlock* split = source->splitBefore(split_point);

  expectEdgeCount(source, first_target, 0);
  expectEdgeCount(source, second_target, 0);
  expectEdgeCount(source, split, 1);
  expectEdgeCount(split, first_target, 1);
  expectEdgeCount(split, second_target, 1);
  expectEdgeCount(other, first_target, 1);
  EXPECT_EQ(first_phi->phiPredecessor(0), split);
  EXPECT_EQ(first_phi->phiInput(0)->getConstant(), 10);
  EXPECT_EQ(first_phi->phiPredecessor(1), other);
  EXPECT_EQ(first_phi->phiInput(1)->getConstant(), 20);
  EXPECT_EQ(second_phi->phiPredecessor(0), split);
  EXPECT_EQ(second_phi->phiInput(0)->getConstant(), 30);
}

TEST(LIRBlockTest, PredecessorAccessors) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* second = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  BasicBlock* loop = function.allocateBasicBlock();
  first->addSuccessor(join);
  second->addSuccessor(join);
  loop->addSuccessor(loop);

  EXPECT_EQ(join->numPredecessors(), 2);
  EXPECT_EQ(join->predecessor(0), first);
  EXPECT_EQ(join->predecessor(1), second);
  EXPECT_THROW(join->predecessor(2), std::runtime_error);
  expectIncomingEdge(join->incomingEdge(0), first, join, 0);
  expectIncomingEdge(join->incomingEdge(1), second, join, 1);

  EXPECT_EQ(loop->numPredecessors(), 1);
  EXPECT_EQ(loop->predecessor(0), loop);
  expectIncomingEdge(loop->incomingEdge(0), loop, loop, 0);

  Instruction* loop_phi = loop->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  EXPECT_EQ(loop_phi->getNumInputs(), 1);
}

TEST(LIRBlockTest, ReplacePredecessorPreservesPhiValues) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* middle = function.allocateBasicBlock();
  BasicBlock* last = function.allocateBasicBlock();
  BasicBlock* replacement_first = function.allocateBasicBlock();
  BasicBlock* replacement_middle = function.allocateBasicBlock();
  BasicBlock* replacement_last = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  IncomingEdge first_edge = first->addSuccessor(join);
  IncomingEdge middle_edge = middle->addSuccessor(join);
  IncomingEdge last_edge = last->addSuccessor(join);
  Instruction* phi = join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(phi, first_edge, 10);
  addImmediatePhiInput(phi, middle_edge, 20);
  addImmediatePhiInput(phi, last_edge, 30);

  join->replacePredecessor(0, replacement_first);
  join->replacePredecessor(1, replacement_middle);
  join->replacePredecessor(2, replacement_last);

  EXPECT_EQ(join->predecessor(0), replacement_first);
  EXPECT_EQ(join->predecessor(1), replacement_middle);
  EXPECT_EQ(join->predecessor(2), replacement_last);
  EXPECT_TRUE(first->successors().empty());
  EXPECT_TRUE(middle->successors().empty());
  EXPECT_TRUE(last->successors().empty());
  EXPECT_EQ(replacement_first->successors(), (std::vector<BasicBlock*>{join}));
  EXPECT_EQ(replacement_middle->successors(), (std::vector<BasicBlock*>{join}));
  EXPECT_EQ(replacement_last->successors(), (std::vector<BasicBlock*>{join}));
  EXPECT_EQ(phi->phiPredecessor(0), replacement_first);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 10);
  EXPECT_EQ(phi->phiPredecessor(1), replacement_middle);
  EXPECT_EQ(phi->phiInput(1)->getConstant(), 20);
  EXPECT_EQ(phi->phiPredecessor(2), replacement_last);
  EXPECT_EQ(phi->phiInput(2)->getConstant(), 30);
}

TEST(LIRBlockTest, RemovePredecessorRemovesPhiValues) {
  Function function;
  BasicBlock* first = function.allocateBasicBlock();
  BasicBlock* middle = function.allocateBasicBlock();
  BasicBlock* last = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  IncomingEdge first_edge = first->addSuccessor(join);
  IncomingEdge middle_edge = middle->addSuccessor(join);
  IncomingEdge last_edge = last->addSuccessor(join);
  Instruction* first_phi =
      join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(first_phi, first_edge, 10);
  addImmediatePhiInput(first_phi, middle_edge, 20);
  addImmediatePhiInput(first_phi, last_edge, 30);
  Instruction* second_phi =
      join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  addImmediatePhiInput(second_phi, first_edge, 100);
  addImmediatePhiInput(second_phi, middle_edge, 200);
  addImmediatePhiInput(second_phi, last_edge, 300);
  auto expect_phi_value = [](const Instruction* phi,
                             size_t slot,
                             const BasicBlock* predecessor,
                             int64_t expected) {
    EXPECT_EQ(phi->phiPredecessor(slot), predecessor);
    const auto* value = phi->phiInput(slot);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->getConstant(), expected);
  };

  join->removePredecessor(1);

  EXPECT_TRUE(middle->successors().empty());
  EXPECT_EQ(join->predecessors(), (std::vector<BasicBlock*>{first, last}));
  EXPECT_EQ(first_phi->numPhiInputs(), 2);
  expect_phi_value(first_phi, 0, first, 10);
  expect_phi_value(first_phi, 1, last, 30);
  EXPECT_EQ(second_phi->numPhiInputs(), 2);
  expect_phi_value(second_phi, 0, first, 100);
  expect_phi_value(second_phi, 1, last, 300);

  join->removePredecessor(1);

  EXPECT_TRUE(last->successors().empty());
  EXPECT_EQ(join->predecessors(), (std::vector<BasicBlock*>{first}));
  EXPECT_EQ(first_phi->numPhiInputs(), 1);
  expect_phi_value(first_phi, 0, first, 10);
  EXPECT_EQ(second_phi->numPhiInputs(), 1);
  expect_phi_value(second_phi, 0, first, 100);

  join->removePredecessor(0);

  EXPECT_TRUE(first->successors().empty());
  EXPECT_TRUE(join->predecessors().empty());
  EXPECT_EQ(first_phi->numPhiInputs(), 0);
  EXPECT_EQ(second_phi->numPhiInputs(), 0);
  EXPECT_THROW(join->removePredecessor(0), std::runtime_error);
}

TEST(LIRBlockTest, BatchRemovePredecessorsCompactsPhiValues) {
  struct Scenario {
    const char* name;
    std::vector<bool> remove;
  };
  const std::vector<Scenario> scenarios{
      {"none", {false, false, false, false, false}},
      {"alternating", {false, true, false, true, false}},
      {"prefix", {true, true, false, false, false}},
      {"suffix", {false, false, false, true, true}},
      {"all", {true, true, true, true, true}},
  };

  for (const Scenario& scenario : scenarios) {
    SCOPED_TRACE(scenario.name);
    Function function;
    std::vector<BasicBlock*> predecessors;
    for (size_t index = 0; index < scenario.remove.size(); ++index) {
      predecessors.push_back(function.allocateBasicBlock());
    }
    BasicBlock* join = function.allocateBasicBlock();
    std::vector<IncomingEdge> edges;
    for (BasicBlock* predecessor : predecessors) {
      edges.push_back(predecessor->addSuccessor(join));
    }

    Instruction* first_phi =
        join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
    Instruction* second_phi =
        join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
    for (size_t index = 0; index < edges.size(); ++index) {
      addImmediatePhiInput(first_phi, edges[index], 10 + index);
      addImmediatePhiInput(second_phi, edges[index], 100 + index);
    }

    size_t predicate_calls = 0;
    join->removePredecessorsIf([&](BasicBlock* predecessor) {
      ++predicate_calls;
      const auto position =
          std::find(predecessors.begin(), predecessors.end(), predecessor);
      return scenario.remove.at(std::distance(predecessors.begin(), position));
    });
    EXPECT_EQ(predicate_calls, predecessors.size());

    std::vector<BasicBlock*> expected_predecessors;
    std::vector<uint64_t> expected_first_values;
    std::vector<uint64_t> expected_second_values;
    for (size_t index = 0; index < predecessors.size(); ++index) {
      if (scenario.remove[index]) {
        EXPECT_TRUE(predecessors[index]->successors().empty());
        continue;
      }
      EXPECT_EQ(
          predecessors[index]->successors(), (std::vector<BasicBlock*>{join}));
      expected_predecessors.push_back(predecessors[index]);
      expected_first_values.push_back(10 + index);
      expected_second_values.push_back(100 + index);
    }

    EXPECT_EQ(join->predecessors(), expected_predecessors);
    EXPECT_EQ(first_phi->numPhiInputs(), expected_predecessors.size());
    EXPECT_EQ(second_phi->numPhiInputs(), expected_predecessors.size());
    std::vector<uint64_t> first_values;
    std::vector<uint64_t> second_values;
    for (size_t index = 0; index < join->numPredecessors(); ++index) {
      lir::Operand* first_value = first_phi->phiInput(index);
      lir::Operand* second_value = second_phi->phiInput(index);
      ASSERT_NE(first_value, nullptr);
      ASSERT_NE(second_value, nullptr);
      EXPECT_EQ(first_value->instr(), first_phi);
      EXPECT_EQ(second_value->instr(), second_phi);
      first_values.push_back(first_value->getConstant());
      second_values.push_back(second_value->getConstant());
    }
    EXPECT_EQ(first_values, expected_first_values);
    EXPECT_EQ(second_values, expected_second_values);
  }
}

TEST(LIRBlockTest, BatchRemovePredecessorsHandlesDuplicateEdges) {
  Function function;
  BasicBlock* removed = function.allocateBasicBlock();
  BasicBlock* kept = function.allocateBasicBlock();
  BasicBlock* join = function.allocateBasicBlock();
  std::vector<IncomingEdge> edges{
      removed->addSuccessor(join),
      kept->addSuccessor(join),
      removed->addSuccessor(join),
  };
  Instruction* phi = join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  for (size_t index = 0; index < edges.size(); ++index) {
    addImmediatePhiInput(phi, edges[index], 10 + index);
  }

  join->removePredecessorsIf(
      [removed](BasicBlock* predecessor) { return predecessor == removed; });

  EXPECT_TRUE(removed->successors().empty());
  EXPECT_EQ(kept->successors(), (std::vector<BasicBlock*>{join}));
  EXPECT_EQ(join->predecessors(), (std::vector<BasicBlock*>{kept}));
  ASSERT_EQ(phi->numPhiInputs(), 1);
  EXPECT_EQ(phi->phiInput(0)->getConstant(), 11);
}

class LIRGeneratorTest : public RuntimeTest {
 public:
  std::unique_ptr<Function> getLIRFunction(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals)) {
      return nullptr;
    }

    if (!PyDict_CheckExact(func->func_builtins)) {
      return nullptr;
    }

    // The returned LIR keeps references into the HIR function and CodeRuntime
    // (e.g. the printer emits the originating HIR instruction as a comment), so
    // they must outlive it. Retain them for the lifetime of the fixture.
    std::unique_ptr<hir::Function>& irfunc =
        hir_funcs_.emplace_back(buildHIR(func));

    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

    std::unique_ptr<CodeRuntime>& runtime =
        runtimes_.emplace_back(std::make_unique<CodeRuntime>(func));
    runtime->setReifier(Ref<>::create(irfunc->env.reifier));

    codegen::Environ env;
    env.reifier = irfunc->env.reifier;
    env.ctx = getContext();
    env.code_rt = runtime.get();

    LIRGenerator lir_gen(irfunc.get(), &env);

    auto lir_func = lir_gen.translateFunction();

    lir_func->sortBasicBlocks();
    return lir_func;
  }

  std::string getLIRString(PyObject* func_obj) {
    auto lir_func = getLIRFunction(func_obj);
    if (!lir_func) {
      return "";
    }
    std::stringstream ss;
    ss << *lir_func << '\n';
    return ss.str();
  }

  std::string removeCommentsAndWhitespace(const std::string& input_s) {
    std::istringstream iss(input_s);
    std::string line;
    std::string output_s;
    while (std::getline(iss, line)) {
      if (line.length() == 0) {
        // skip blank lines
        continue;
      } else if (line.length() > 0 && line.at(0) == '#') {
        // skip comments
        continue;
      } else {
        output_s += line + '\n';
      }
    }
    return output_s;
  }

  void expectResumeEntryDispatchIsNotAnSSAPredecessor(
      PyObject* pyfunc,
      bool use_spill_allocator) {
    auto lir_func = getLIRFunction(pyfunc);
    BasicBlock* resume_entry = lir_func->resumeEntryBlock();
    ASSERT_NE(resume_entry, nullptr);
    EXPECT_TRUE(resume_entry->predecessors().empty());
    EXPECT_TRUE(resume_entry->successors().empty());
    EXPECT_EQ(
        std::count(
            lir_func->basicBlocks().begin(),
            lir_func->basicBlocks().end(),
            resume_entry),
        0);

    std::vector<BasicBlock*> resume_blocks;
    bool saw_yield_from = false;
    for (BasicBlock* block : lir_func->basicBlocks()) {
      for (const auto& instruction : block->instructions()) {
        if (instruction->opcode() == Opcode::kResumeGenYield) {
          resume_blocks.push_back(block);
        } else if (instruction->opcode() == Opcode::kStoreGenYieldFromPoint) {
          saw_yield_from = true;
        }
      }
    }
    // `yield from` contributes two resume points in addition to the two
    // explicit yields.
    ASSERT_EQ(resume_blocks.size(), 4);
    EXPECT_TRUE(saw_yield_from);
    for (BasicBlock* block : resume_blocks) {
      ASSERT_EQ(block->numPredecessors(), 1);
      BasicBlock* yield_block = block->predecessor(0);
      EXPECT_NE(yield_block, resume_entry);
      ASSERT_EQ(yield_block->successors().size(), 2);
      EXPECT_EQ(yield_block->successors().back(), block);
      ASSERT_NE(yield_block->getLastInstr(), nullptr);
      EXPECT_EQ(
          yield_block->getLastInstr()->opcode(), Opcode::kBranchToYieldExit);
    }

    if (use_spill_allocator) {
      SpillAllocator{lir_func.get()}.run();
    } else {
      LinearScanAllocator{lir_func.get()}.run();
    }
    for (BasicBlock* block : resume_blocks) {
      EXPECT_TRUE(block->predecessors().empty());
    }

    codegen::Environ env;
    PostRegAllocRewrite{lir_func.get(), &env}.run();
    std::ostringstream verification_errors;
    EXPECT_TRUE(
        verifyPostRegAllocInvariants(lir_func.get(), verification_errors))
        << verification_errors.str();

    PopulateResumeEntryBlock(resume_entry, 0);
    EXPECT_TRUE(resume_entry->successors().empty());
    Instruction* dispatch = resume_entry->getLastInstr();
    ASSERT_NE(dispatch, nullptr);
    EXPECT_EQ(dispatch->opcode(), Opcode::kBranch);
    ASSERT_EQ(dispatch->getNumInputs(), 1);
    EXPECT_TRUE(dispatch->getInput(0)->isInd());

    lir_func->basicBlocks().push_back(resume_entry);
    verification_errors.str("");
    EXPECT_TRUE(
        verifyPostRegAllocInvariants(lir_func.get(), verification_errors))
        << verification_errors.str();
  }

  void TearDown() override {
    // These hold Python references and must be destroyed before the base
    // fixture finalizes the interpreter.
    runtimes_.clear();
    hir_funcs_.clear();
    RuntimeTest::TearDown();
  }

 private:
  // Keep the HIR functions and runtimes referenced by LIR produced in this
  // fixture alive until the test finishes.
  std::vector<std::unique_ptr<hir::Function>> hir_funcs_;
  std::vector<std::unique_ptr<CodeRuntime>> runtimes_;
};

TEST_F(LIRGeneratorTest, GeneratedPhiInputsFollowPredecessors) {
  const char* src = R"(
def func(x):
  value = 10
  if x:
    value = 20
  return value
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  expectPhiInputsFollowPredecessors(findPhiWithInputCount(lir_func.get(), 2));
}

TEST_F(LIRGeneratorTest, GeneratedPhiCoversDuplicateIncomingEdges) {
  const char* hir = R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadConst<NoneType>
    CondBranch<1, 1> v0
  }

  bb 1 {
    v2 = Phi<0> v1
    Return v2
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  codegen::Environ env;
  env.ctx = getContext();
  LIRGenerator lir_gen(irfunc.get(), &env);
  auto lir_func = lir_gen.translateFunction();

  Instruction* phi = findPhiWithInputCount(lir_func.get(), 2);
  ASSERT_NE(phi, nullptr);
  expectPhiInputsFollowPredecessors(phi);
  EXPECT_EQ(phi->phiPredecessor(0), phi->phiPredecessor(1));
  ASSERT_TRUE(phi->phiInput(0)->isLinked());
  ASSERT_TRUE(phi->phiInput(1)->isLinked());
  EXPECT_EQ(
      phi->phiInput(0)->getLinkedInstr(), phi->phiInput(1)->getLinkedInstr());
}

TEST_F(LIRGeneratorTest, GeneratedPhiRejectsDuplicateHIRPredecessors) {
  const char* hir = R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadConst<NoneType>
    CondBranch<1, 1> v0
  }

  bb 1 {
    v2 = Phi<0, 0> v1 v1
    Return v2
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  codegen::Environ env;
  env.ctx = getContext();
  LIRGenerator lir_gen(irfunc.get(), &env);
  EXPECT_THROW(lir_gen.translateFunction(), std::runtime_error);
}

TEST_F(LIRGeneratorTest, GeneratorExitPhiInputsFollowPredecessors) {
  const char* src = R"(
def func():
  yield 1
  yield 2
  return 3
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  expectPhiInputsFollowPredecessors(findPhiWithInputCount(lir_func.get(), 4));
}

TEST_F(LIRGeneratorTest, GeneratorWithoutReturnOmitsReturnPhi) {
  const char* src = R"(
def func():
  yield 1
  raise RuntimeError("expected")
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  Instruction* epilogue_end = nullptr;
  for (BasicBlock* block : lir_func->basicBlocks()) {
    for (const auto& instruction : block->instructions()) {
      if (instruction->isPhi()) {
        expectPhiInputsFollowPredecessors(instruction.get());
        EXPECT_GT(instruction->numPhiInputs(), 0);
      } else if (instruction->opcode() == Opcode::kEpilogueEnd) {
        epilogue_end = instruction.get();
      }
    }
  }

  ASSERT_NE(epilogue_end, nullptr);
  ASSERT_EQ(epilogue_end->getNumInputs(), 1);
  EXPECT_TRUE(epilogue_end->getInput(0)->isLinked());
  Instruction* epilogue_phi = epilogue_end->getInput(0)->getLinkedInstr();
  ASSERT_TRUE(epilogue_phi->isPhi());
  ASSERT_EQ(epilogue_phi->numPhiInputs(), 2);
  for (size_t slot = 0; slot < epilogue_phi->numPhiInputs(); ++slot) {
    Instruction* terminator =
        epilogue_phi->phiPredecessor(slot)->getLastInstr();
    ASSERT_NE(terminator, nullptr);
    EXPECT_EQ(terminator->opcode(), Opcode::kBranchToYieldExit);
  }
}

TEST_F(LIRGeneratorTest, LinearScanResumeEntryDispatchIsNotAnSSAPredecessor) {
  const char* src = R"(
def func(items):
  yield 1
  yield from items
  yield 2
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  expectResumeEntryDispatchIsNotAnSSAPredecessor(pyfunc.get(), false);
}

TEST_F(LIRGeneratorTest, SpillResumeEntryDispatchIsNotAnSSAPredecessor) {
  const char* src = R"(
def func(items):
  yield 1
  yield from items
  yield 2
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  expectResumeEntryDispatchIsNotAnSSAPredecessor(pyfunc.get(), true);
}

TEST_F(LIRGeneratorTest, StaticLoadInteger) {
  const char* pycode = R"(
from __static__ import int64

def f() -> int64:
  d: int64 = 12
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 12));
}

TEST_F(LIRGeneratorTest, StaticLoadDouble) {
  const char* pycode = R"(
from __static__ import double

def f() -> double:
  d: double = 3.1415
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 4614256447914709615ULL));
}

TEST_F(LIRGeneratorTest, StaticBoxDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 3.1415
  return box(d)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 4614256447914709615ULL));
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kCall).outType(DataType::kObject));
}

TEST_F(LIRGeneratorTest, StaticAddDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 1.14
  e: double = 2.00
  return box(d + e)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // `d + e` on doubles should lower to an Fadd.
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kFadd));
}

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_Fallthrough) {
  const char* src = R"(
def func2(x):
  y = 0
  if x:
    y = 100
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Load %5, 8(0x8)
             %10 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %10, %9, %8
                   CondBranch %10, BB%14, BB%13

BB %13 - preds: %7

BB %14 - preds: %7
             %15 = Load %5, 16(0x10)

BB %16 - preds: %13 %14
             %17 = Phi (BB%14, %15), (BB%13, %9)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_CondBranch) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %9, %8
                   CondBranch %9, BB%16, BB%12

BB %12 - preds: %7
             %13 = Load %5, 16(0x10)
                   Call {1}({1:#x}), %13
                   Return %13

BB %16 - preds: %7
             %17 = Load %5, 8(0x8)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %12 %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

TEST_F(LIRGeneratorTest, ParserDataTypeTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %7 %10
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10
       %8:Object = Load [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserAlignsPhiPairsWithIncomingEdges) {
  const char* lir_str = R"(Function:
BB %3
      %12:Object = Phi (BB%1, %11:Object), (BB%0, %10:Object)
                   Return %12:Object

BB %0 - succs: %3
      %10:Object = Move 10(0xa):64bit
                   Branch BB%3

BB %1 - succs: %3
      %11:Object = Move 20(0x14):64bit
                   Branch BB%3

)";

  const auto expect_aligned_phi = [](Function* function) {
    Instruction* phi = findPhiWithInputCount(function, 2);
    ASSERT_NE(phi, nullptr);
    std::vector<std::pair<int, int>> incoming_values;
    for (size_t index = 0; index < phi->numPhiInputs(); ++index) {
      const lir::Operand* value = phi->phiInput(index);
      ASSERT_NE(value, nullptr);
      ASSERT_TRUE(value->isLinked());
      incoming_values.emplace_back(
          phi->phiPredecessor(index)->id(), value->getLinkedInstr()->id());
    }
    const std::vector<std::pair<int, int>> expected{{0, 10}, {1, 11}};
    EXPECT_EQ(incoming_values, expected);
  };

  auto parsed = Parser().parse(lir_str);
  expect_aligned_phi(parsed.get());

  std::stringstream printed;
  printed << *parsed;
  auto reparsed = Parser().parse(printed.str());
  expect_aligned_phi(reparsed.get());
}

TEST_F(LIRGeneratorTest, ParserRejectsFlatPhiSyntax) {
  EXPECT_THROW(
      Parser().parse(R"(Function:
BB %0
      %1:Object = Phi BB%0, 1(0x1):Object

)"),
      ParserException);
}

TEST_F(LIRGeneratorTest, ParserRejectsPhiLabelValue) {
  EXPECT_THROW(
      Parser().parse(R"(Function:
BB %0
      %1:Object = Phi (BB%0, BB%0)

)"),
      ParserException);
}

TEST_F(LIRGeneratorTest, ParserAlignsPhiPairsWithDuplicateEdges) {
  const char* lir_str = R"(Function:
BB %0 - succs: %1 %1
      %10:Object = Move 10(0xa):64bit
      %11:Object = Move 20(0x14):64bit
                   CondBranch %10:Object, BB%1, BB%1

BB %1
      %12:Object = Phi (BB%0, %10:Object), (BB%0, %11:Object)
                   Return %12:Object

)";

  auto parsed = Parser().parse(lir_str);
  Instruction* phi = findPhiWithInputCount(parsed.get(), 2);
  ASSERT_NE(phi, nullptr);
  EXPECT_EQ(phi->phiPredecessor(0), phi->phiPredecessor(1));
  ASSERT_TRUE(phi->phiInput(0)->isLinked());
  ASSERT_TRUE(phi->phiInput(1)->isLinked());
  EXPECT_EQ(phi->phiInput(0)->getLinkedInstr()->id(), 10);
  EXPECT_EQ(phi->phiInput(1)->getLinkedInstr()->id(), 11);
}

TEST_F(LIRGeneratorTest, ParserMemIndTest) {
  auto lir_str = fmt::format(
      R"(Function:
BB %0
        %1:64bit = Bind {}:Object
        %2:64bit = Load [{}:Object + {}:Object * 8 + 0x8]:Object
        %3:64bit = Load [%2:64bit + 0x3]:Object
        %4:64bit = Load [%2:64bit + %3:64bit * 16]:Object
[%4:64bit - 0x16]:Object = Store %1:64bit

)",
      PhyLocation{5, 64},
      PhyLocation{5, 64},
      PhyLocation{4, 64},
      PhyLocation{0, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserTest) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = removeCommentsAndWhitespace(getLIRString(pyfunc.get()));

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream first_print;
  parsed_func->sortBasicBlocks();
  first_print << *parsed_func;
  ASSERT_EQ(lir_str, removeCommentsAndWhitespace(first_print.str()));

  auto reparsed_func = Parser().parse(first_print.str());
  std::stringstream second_print;
  reparsed_func->sortBasicBlocks();
  second_print << *reparsed_func;
  ASSERT_EQ(
      removeCommentsAndWhitespace(first_print.str()),
      removeCommentsAndWhitespace(second_print.str()));
}

TEST_F(LIRGeneratorTest, ParserCanBeReused) {
  const char* lir_str = R"(Function:
BB %0
                   Return 0(0x0):Object

)";

  Parser parser;
  auto first = parser.parse(lir_str);
  auto second = parser.parse(lir_str);
  ASSERT_EQ(first->basicBlocks().size(), 1);
  ASSERT_EQ(second->basicBlocks().size(), 1);
}

TEST_F(LIRGeneratorTest, ParserRejectsParenthesizedNonPhiInput) {
  EXPECT_THROW(
      Parser().parse(R"(Function:
BB %0
       %1:Object = Move (BB%0, 0(0x0):Object)

)"),
      ParserException);
}

TEST_F(LIRGeneratorTest, ParserSectionTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - section: hot
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10 - section: .coldtext
       %8:Object = Load [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7 - section: hot

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  ASSERT_EQ(parsed_func->basicBlocks().size(), 3);
  ASSERT_EQ(
      parsed_func->basicBlocks()[0]->section(), codegen::CodeSection::kHot);
  ASSERT_EQ(
      parsed_func->basicBlocks()[1]->section(), codegen::CodeSection::kCold);
  ASSERT_EQ(
      parsed_func->basicBlocks()[2]->section(), codegen::CodeSection::kHot);
}

template <typename... Args>
std::string formatMemoryIndirect(Args&&... args) {
  MemoryIndirect im(nullptr);
  im.setMemoryIndirect(std::forward<Args>(args)...);
  return fmt::format("{}", im);
}

TEST(LIRTest, MemoryIndirectTests) {
  auto base = codegen::ARGUMENT_REGS[3];
  auto index = codegen::ARGUMENT_REGS[2];

  ASSERT_EQ(fmt::format("[{}:Object]", base), formatMemoryIndirect(base.loc));
  ASSERT_EQ(
      fmt::format("[{}:Object + 0x7fff]", base),
      formatMemoryIndirect(base.loc, 0x7fff));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 4]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 2));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object + 0x100]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0, 0x100));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 2 + 0x1000]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 1, 0x1000));
}

extern "C" uint64_t __Invoke_PyTuple_Check(PyObject* obj);

TEST_F(LIRGeneratorTest, CondBranchCheckTypeEmitsCallToSubclassCheck) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadArg<0>
    CondBranchCheckType<1, 2, Tuple> v0
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Branch<2>
  }

  bb 2 {
    Return v0
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  codegen::Environ env;

  env.ctx = getContext();

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.translateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';

  std::string lir_str = ss.str();
  lir_str.erase(
      std::remove(lir_str.begin(), lir_str.end(), '\n'), lir_str.end());

  std::string lir_expected_re = fmt::format(
      R"(# CondBranchCheckType<1, 3, Tuple> v1\s+%\d+:8bit = Call {0}\({0:#x}\):64bit, %\d+:Object\s+CondBranch %\d+:8bit, BB%\d+, BB%\d+)",
      reinterpret_cast<uint64_t>(__Invoke_PyTuple_Check));

  std::regex re(lir_expected_re);
  if (!std::regex_search(lir_str, re)) {
    FAIL() << "Couldn't find expected string \n"
           << lir_expected_re << '\n'
           << "In:\n"
           << lir_str << '\n';
  }
}

TEST_F(LIRGeneratorTest, UnreachableFollowsBottomType) {
  const char* hir_source = R"(fun test {
  bb 0 {
    v7 = LoadConst<Nullptr>
    v8 = CheckVar<"a"> v7 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v7
      }
    }
    Unreachable
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir_source);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  codegen::Environ env;
  CodeRuntime code_runtime{irfunc->code, irfunc->builtins, irfunc->globals};

  env.ctx = getContext();
  env.code_rt = &code_runtime;

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.translateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';
#if PY_VERSION_HEX >= 0x030E0000
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
                   EpilogueEnd


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation{7, 64}
#else
      PhyLocation{0, 64}
#endif
  );
#else
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
                   EpilogueEnd


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation{7, 64}
#else
      PhyLocation{0, 64}
#endif
  );
#endif
  ASSERT_EQ(ss.str(), lir_expected.c_str());
}

TEST_F(LIRGeneratorTest, AttrCachesOff) {
  getMutableConfig().attr_caches = false;

  const char* src = R"(
import sys

def func():
  return sys.argv
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  auto slow_addr = reinterpret_cast<uint64_t>(PyObject_GetAttr);

  EXPECT_FALSE(getConfig().attr_caches);

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kCall).inAddr(0, slow_addr))
      << "Should be calling out to PyObject_GetAttr as inline caches are "
         "disabled";
  // An inline cache dispatches through a function pointer loaded out of the
  // cache, so it shows up as a Call whose callee is defined by a Load rather
  // than as an immediate address. There should be no such call here.
  EXPECT_NO_LIR(
      Query(*lir_func).opcode(Opcode::kCall).inDefOpcode(0, Opcode::kLoad))
      << "Should not be emitting an indirect call through an inline cache as "
         "inline caches are disabled";
}

TEST_F(LIRGeneratorTest, AttrCachesOn) {
  if constexpr (kFreeThreadedBuild) {
    SKIP(
        "T250369692: Attribute inline caches are not supported on "
        "free-threaded builds");
  }
  getMutableConfig().attr_caches = true;

  const char* src = R"(
def func(o):
  return o.attr
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  EXPECT_TRUE(getConfig().attr_caches);

#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  // The cache picks its own entry point, so codegen loads the callee out of the
  // cache and calls it indirectly instead of baking an address into the
  // instruction stream.
  EXPECT_LIR(
      Query(*lir_func).opcode(Opcode::kCall).inDefOpcode(0, Opcode::kLoad))
      << "Should be calling indirectly through the inline cache's target";
#else
  // Without target promotion the callee is a fixed address again.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kCall)
                 .inAddr(0, reinterpret_cast<uint64_t>(LoadAttrCache::invoke)))
      << "Should be calling LoadAttrCache::invoke directly";
  EXPECT_NO_LIR(
      Query(*lir_func).opcode(Opcode::kCall).inDefOpcode(0, Opcode::kLoad))
      << "Should not emit an indirect call with target promotion disabled";
#endif
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Opcode::kCall)
                    .inAddr(0, reinterpret_cast<uint64_t>(PyObject_GetAttr)))
      << "Should not be calling PyObject_GetAttr directly as inline caches are "
         "enabled";
}

TEST_F(LIRGeneratorTest, LoadMethodCacheOff) {
  getMutableConfig().attr_caches = false;

  const char* src = R"(
def func(o):
  return o.method()
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  EXPECT_FALSE(getConfig().attr_caches);
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kCall)
                 .inAddr(0, reinterpret_cast<uint64_t>(rt::getMethod)))
      << "Should call rt::getMethod when inline caches are disabled";
  EXPECT_NO_LIR(
      Query(*lir_func)
          .opcode(Opcode::kCall)
          .inAddr(0, reinterpret_cast<uint64_t>(LoadMethodCache::lookupHelper)))
      << "Should not call LoadMethodCache::lookupHelper when inline caches are "
         "disabled";
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kLoadSecondCallResult));
}

TEST_F(LIRGeneratorTest, LoadMethodCacheOn) {
  if constexpr (kFreeThreadedBuild) {
    SKIP(
        "T250369692: Attribute inline caches are not supported on "
        "free-threaded builds");
  }
  getMutableConfig().attr_caches = true;

  const char* src = R"(
def func(o):
  return o.method()
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  EXPECT_TRUE(getConfig().attr_caches);
  EXPECT_LIR(
      Query(*lir_func)
          .opcode(Opcode::kCall)
          .inAddr(0, reinterpret_cast<uint64_t>(LoadMethodCache::lookupHelper)))
      << "Should call LoadMethodCache::lookupHelper when inline caches are "
         "enabled";
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Opcode::kCall)
                    .inAddr(0, reinterpret_cast<uint64_t>(rt::getMethod)))
      << "Should not call rt::getMethod when inline caches are enabled";
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kLoadSecondCallResult));
}

TEST_F(LIRGeneratorTest, LoadEvalBreakerUsesMoveRelaxed) {
  // Backward jumps (loop back-edges) emit LoadEvalBreaker in HIR to check
  // whether the interpreter needs to handle pending events. This should lower
  // to MoveRelaxed in LIR.
  const char* src = R"(
def func():
  x = 0
  while x < 10:
    x += 1
  return x
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kMoveRelaxed))
      << "LoadEvalBreaker should lower to MoveRelaxed";
}

TEST_F(LIRGeneratorTest, ListDynamicIndexLoadStoreUsesScaledArrayLIR) {
  const char* src = R"(
import os

def func(value):
  xs = [1, 2, 3]
  i = 1 if os.argv else 2
  old = xs[i]
  xs[i] = value
  return old
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  if constexpr (kFreeThreadedBuild) {
    auto lir_func = getLIRFunction(pyfunc.get());
    EXPECT_LIR(Query(*lir_func)
                   .opcode(Opcode::kCall)
                   .inAddr(0, reinterpret_cast<uint64_t>(rt::listSubscript)));
    EXPECT_LIR(Query(*lir_func)
                   .opcode(Opcode::kCall)
                   .inAddr(0, reinterpret_cast<uint64_t>(PyObject_SetItem)));
    return;
  }

  auto lir_str = getLIRString(pyfunc.get());
  const std::regex scaled_array_load{
      R"(:Object = Load \[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object)"};
  const std::regex scaled_array_store{
      R"(\[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object = Store %\d+:Object)"};

  EXPECT_TRUE(std::regex_search(lir_str, scaled_array_load)) << lir_str;
  EXPECT_TRUE(std::regex_search(lir_str, scaled_array_store)) << lir_str;
}

} // namespace cinderx
