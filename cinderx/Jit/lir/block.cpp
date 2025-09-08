// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/block.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/lir/function.h"

namespace jit::lir {

BasicBlock::BasicBlock(Function* func) : id_(func->allocateId()), func_(func) {}

int BasicBlock::id() const {
  return id_;
}

void BasicBlock::setId(int id) {
  id_ = id;
}

Function* BasicBlock::function() {
  return func_;
}

const Function* BasicBlock::function() const {
  return func_;
}

void BasicBlock::addSuccessor(BasicBlock* bb) {
  successors_.push_back(bb);
  bb->predecessors_.push_back(this);
}

void BasicBlock::setSuccessor(size_t index, BasicBlock* bb) {
  JIT_CHECK(index < successors_.size(), "Index out of range");
  BasicBlock* old_bb = successors_[index];
  std::vector<BasicBlock*>& old_preds = old_bb->predecessors_;
  old_preds.erase(std::find(old_preds.begin(), old_preds.end(), this));

  successors_[index] = bb;
  bb->predecessors_.push_back(this);
}

std::vector<BasicBlock*>& BasicBlock::successors() {
  return successors_;
}

const std::vector<BasicBlock*>& BasicBlock::successors() const {
  return successors_;
}

void BasicBlock::swapSuccessors() {
  if (successors_.size() < 2) {
    return;
  }

  JIT_DCHECK(successors_.size() == 2, "Should at most have two successors.");
  std::swap(successors_[0], successors_[1]);
}

BasicBlock* BasicBlock::getTrueSuccessor() const {
  return successors_[0];
}

BasicBlock* BasicBlock::getFalseSuccessor() const {
  return successors_[1];
}

std::vector<BasicBlock*>& BasicBlock::predecessors() {
  return predecessors_;
}

const std::vector<BasicBlock*>& BasicBlock::predecessors() const {
  return predecessors_;
}

void BasicBlock::appendInstr(std::unique_ptr<Instruction> instr) {
  instrs_.emplace_back(std::move(instr));
}

std::unique_ptr<Instruction> BasicBlock::removeInstr(instr_iter_t iter) {
  auto instr = std::move(*iter);
  instrs_.erase(iter);
  return instr;
}

BasicBlock::InstrList& BasicBlock::instructions() {
  return instrs_;
}

const BasicBlock::InstrList& BasicBlock::instructions() const {
  return instrs_;
}

bool BasicBlock::isEmpty() const {
  return instrs_.empty();
}

size_t BasicBlock::getNumInstrs() const {
  return instrs_.size();
}

Instruction* BasicBlock::getFirstInstr() {
  return instrs_.empty() ? nullptr : instrs_.begin()->get();
}

const Instruction* BasicBlock::getFirstInstr() const {
  return instrs_.empty() ? nullptr : instrs_.begin()->get();
}

Instruction* BasicBlock::getLastInstr() {
  return instrs_.empty() ? nullptr : instrs_.rbegin()->get();
}

const Instruction* BasicBlock::getLastInstr() const {
  return instrs_.empty() ? nullptr : instrs_.rbegin()->get();
}

instr_iter_t BasicBlock::getLastInstrIter() {
  return instrs_.empty() ? instrs_.end() : std::prev(instrs_.end());
}

BasicBlock* BasicBlock::insertBasicBlockBetween(BasicBlock* block) {
  auto i = std::find(successors_.begin(), successors_.end(), block);
  JIT_DCHECK(i != successors_.end(), "block must be one of the successors.");

  auto new_block = func_->allocateBasicBlockAfter(this);
  *i = new_block;
  new_block->predecessors_.push_back(this);

  auto& old_preds = block->predecessors_;
  old_preds.erase(std::find(old_preds.begin(), old_preds.end(), this));

  new_block->addSuccessor(block);

  return new_block;
}

BasicBlock* BasicBlock::splitBefore(Instruction* instr) {
  JIT_CHECK(
      func_ != nullptr, "cannot split block that doesn't belong to a function");
  JIT_CHECK(
      instr->opcode() != Instruction::kPhi, "cannot split block at a phi node");

  // find the instruction
  instr_iter_t it = instrs_.begin();
  while (it != instrs_.end()) {
    if (it->get() == instr) {
      break;
    } else {
      ++it;
    }
  }

  // the instruction should be in the basic block, otherwise we cannot split
  if (it == instrs_.end()) {
    return nullptr;
  }

  auto second_block = func_->allocateBasicBlockAfter(this);
  // move all instructions after iterator
  while (it != instrs_.end()) {
    it->get()->setbasicblock(second_block);
    second_block->appendInstr(std::move(*it));
    it = instrs_.erase(it);
  }

  // fix up successors
  for (auto bb : successors_) {
    // fix up phis in successors
    bb->fixupPhis(this, second_block);
    // update successors of second block
    second_block->successors().push_back(bb);
    replace(
        bb->predecessors().begin(),
        bb->predecessors().end(),
        this,
        second_block);
  }

  // update successors of first block
  successors_.clear();
  // addSuccessor also fixes predecessors of second block
  addSuccessor(second_block);
  return second_block;
}

void BasicBlock::fixupPhis(BasicBlock* old_pred, BasicBlock* new_pred) {
  foreachPhiInstr([&](Instruction* instr) {
    for (size_t i = 0, n = instr->getNumInputs(); i < n; ++i) {
      auto block = instr->getInput(i);
      if (block->type() == Operand::kLabel) {
        if (block->getBasicBlock() == old_pred) {
          static_cast<Operand*>(block)->setBasicBlock(new_pred);
        }
      }
    }
  });
}

codegen::CodeSection BasicBlock::section() const {
  return section_;
}

void BasicBlock::setSection(codegen::CodeSection section) {
  section_ = section;
}

BasicBlock::instr_iter_t BasicBlock::iterator_to(Instruction* instr) {
  for (auto it = instrs_.begin(); it != instrs_.end(); ++it) {
    if (it->get() == instr) {
      return it;
    }
  }
  JIT_ABORT("Instruction not found in list");
}

} // namespace jit::lir
