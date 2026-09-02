// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/block.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/lir/function.h"

namespace cinderx::jit::lir {

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
  appendSuccessor(bb);
  bb->addPredecessor(this);
}

void BasicBlock::setSuccessor(size_t index, BasicBlock* bb) {
  JIT_CHECK(index < successors_.size(), "Index out of range");
  BasicBlock* old_bb = successors_[index];
  if (old_bb == bb) {
    return;
  }

  old_bb->removePredecessor(this);
  successors_[index] = bb;
  bb->addPredecessor(this);
}

void BasicBlock::popSuccessor() {
  JIT_THROW_IF(
      successors_.empty(), "No successor to remove from block {}", id_);
  successors_.back()->removePredecessor(this);
  successors_.pop_back();
}

const std::vector<BasicBlock*>& BasicBlock::successors() const {
  return successors_;
}

void BasicBlock::appendSuccessor(BasicBlock* successor) {
  successors_.push_back(successor);
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

const std::vector<BasicBlock*>& BasicBlock::predecessors() const {
  return predecessors_;
}

size_t BasicBlock::numPredecessors() const {
  return predecessors_.size();
}

BasicBlock* BasicBlock::predecessor(size_t index) const {
  JIT_THROW_IF(
      index >= predecessors_.size(),
      "Predecessor index {} out of range for block {}, has {} predecessors",
      index,
      id_,
      predecessors_.size());
  return predecessors_[index];
}

void BasicBlock::addPredecessor(BasicBlock* predecessor) {
  predecessors_.push_back(predecessor);
}

std::optional<size_t> BasicBlock::findPredecessorIndex(
    const BasicBlock* predecessor) const {
  std::optional<size_t> result;
  for (size_t index = 0; index < predecessors_.size(); ++index) {
    if (predecessors_[index] == predecessor) {
      JIT_THROW_IF(
          result.has_value(),
          "Predecessor block {} lookup is ambiguous, already matched {}",
          predecessor->id(),
          result.value());
      result = index;
    }
  }
  return result;
}

size_t BasicBlock::predecessorIndex(const BasicBlock* predecessor) const {
  std::optional<size_t> index = findPredecessorIndex(predecessor);
  JIT_THROW_IF(
      !index.has_value(), "Predecessor block {} not found", predecessor->id());
  return *index;
}

void BasicBlock::replacePredecessor(
    BasicBlock* predecessor,
    BasicBlock* replacement) {
  predecessors_[predecessorIndex(predecessor)] = replacement;
  foreachPhiInstr([&](Instruction* instr) {
    for (size_t i = 0, n = instr->getNumInputs(); i < n; ++i) {
      Operand* input = instr->getInput(i);
      if (input->type() == Operand::kLabel &&
          input->getBasicBlock() == predecessor) {
        input->setBasicBlock(replacement);
      }
    }
  });
}

void BasicBlock::removePredecessor(BasicBlock* predecessor) {
  const size_t predecessor_index = predecessorIndex(predecessor);
  predecessors_.erase(predecessors_.begin() + predecessor_index);
  foreachPhiInstr([&](Instruction* instr) {
    const int value_index = instr->getOperandIndexByPredecessor(predecessor);
    if (value_index == -1) {
      return;
    }
    instr->removeInput(value_index);
    instr->removeInput(value_index - 1);
  });
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
  block->replacePredecessor(this, new_block);
  *i = new_block;
  new_block->predecessors_.push_back(this);
  new_block->successors_.push_back(block);

  return new_block;
}

BasicBlock* BasicBlock::splitBefore(Instruction* instr) {
  JIT_CHECK(
      func_ != nullptr, "cannot split block that doesn't belong to a function");
  JIT_CHECK(
      instr->opcode() != Opcode::kPhi, "cannot split block at a phi node");

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
    it->get()->setBasicBlock(second_block);
    second_block->appendInstr(std::move(*it));
    it = instrs_.erase(it);
  }

  // fix up successors
  for (auto bb : successors_) {
    bb->replacePredecessor(this, second_block);
    second_block->appendSuccessor(bb);
  }

  // update successors of first block
  successors_.clear();
  // addSuccessor also fixes predecessors of second block
  addSuccessor(second_block);
  return second_block;
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

void BasicBlock::applyPendingAnnotation(Instruction* instr) {
  if (!pending_annotation_.empty()) {
    func_->annotate(instr, std::move(pending_annotation_));
    pending_annotation_.clear();
  }
}

} // namespace cinderx::jit::lir
