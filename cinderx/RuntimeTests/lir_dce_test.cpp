// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/lir/dce.h"

#include <cstdint>
#include <initializer_list>

namespace cinderx::jit::lir {
namespace {

Instruction* allocateLoadImmediate(BasicBlock* block, uint64_t value) {
  return block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{}, Imm{value, DataType::kObject});
}

void expectInstructions(
    BasicBlock* block,
    std::initializer_list<Instruction*> expected) {
  ASSERT_EQ(block->getNumInstrs(), expected.size());
  auto actual = block->instructions().begin();
  for (Instruction* instruction : expected) {
    EXPECT_EQ(actual->get(), instruction);
    ++actual;
  }
}

} // namespace

TEST(LIRDeadCodeEliminationTest, TestEliminateMov) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);

  Instruction* live = allocateLoadImmediate(entry, 1);
  Instruction* branch = entry->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{live},
      Lbl{true_block},
      Lbl{false_block});

  allocateLoadImmediate(entry, 2); /* should be eliminated */

  Instruction* returned_value = true_block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{}, MemImm{reinterpret_cast<void*>(0x5)});
  Instruction* ret =
      true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{returned_value});

  eliminateDeadCode(&function);

  expectInstructions(entry, {live, branch});
  expectInstructions(true_block, {returned_value, ret});
}

TEST(LIRDeadCodeEliminationTest, TestLocalBaseForIndirectNotEliminated) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();

  Instruction* base = allocateLoadImmediate(block, 1);

  allocateLoadImmediate(block, 2); /* should be eliminated */

  Instruction* load =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Ind{base, 0x18});
  Instruction* ret = block->allocateInstr(Opcode::kReturn, nullptr, VReg{load});

  eliminateDeadCode(&function);

  expectInstructions(block, {base, load, ret});
}

TEST(LIRDeadCodeEliminationTest, TestLocalIndexForIndirectNotEliminated) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();

  Instruction* index = allocateLoadImmediate(block, 1);

  allocateLoadImmediate(block, 2); /* should be eliminated */

  Instruction* load = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{}, Ind{PhyLocation{7, 64}, index});
  Instruction* ret = block->allocateInstr(Opcode::kReturn, nullptr, VReg{load});

  eliminateDeadCode(&function);

  expectInstructions(block, {index, load, ret});
}

TEST(
    LIRDeadCodeEliminationTest,
    TestLocalBaseForIndirectNotEliminatedInOutput) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();

  Instruction* source = block->allocateInstr(
      Opcode::kBind,
      nullptr,
      OutVReg{DataType::k64bit},
      PhyReg{PhyLocation{10, 64}, DataType::k64bit});
  Instruction* base = allocateLoadImmediate(block, 0);

  allocateLoadImmediate(block, 1); /* should be eliminated */

  Instruction* store = block->allocateInstr(
      Opcode::kMove, nullptr, OutInd{base, 0x18}, VReg{source});

  eliminateDeadCode(&function);

  expectInstructions(block, {source, base, store});
}

} // namespace cinderx::jit::lir
