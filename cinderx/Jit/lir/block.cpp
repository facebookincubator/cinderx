// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/block.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/lir/function.h"

namespace cinderx::jit::lir {

IncomingEdge::IncomingEdge(BasicBlock* successor, size_t incoming_slot)
    : successor_(successor), incoming_slot_(incoming_slot) {}

BasicBlock* IncomingEdge::predecessor() const {
  return successor_->predecessor(incoming_slot_);
}

BasicBlock* IncomingEdge::successor() const {
  return successor_;
}

size_t IncomingEdge::outgoingSlot() const {
  BasicBlock* const predecessor_block = predecessor();
  size_t occurrence = 0;
  for (size_t earlier = 0; earlier < incoming_slot_; ++earlier) {
    occurrence += successor_->predecessor(earlier) == predecessor_block;
  }
  const auto& successors = predecessor_block->successors();
  for (size_t outgoing_slot = 0; outgoing_slot < successors.size();
       ++outgoing_slot) {
    if (successors[outgoing_slot] != successor_) {
      continue;
    }
    if (occurrence == 0) {
      return outgoing_slot;
    }
    --occurrence;
  }
  JIT_ABORT("Predecessor has no matching successor edge");
}

size_t IncomingEdge::incomingSlot() const {
  return incoming_slot_;
}

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

IncomingEdge BasicBlock::addSuccessor(BasicBlock* bb) {
  const size_t incoming_slot = bb->addPredecessor(this);
  appendSuccessor(bb);
  return IncomingEdge{bb, incoming_slot};
}

void BasicBlock::setSuccessor(
    size_t index,
    size_t old_successor_incoming_slot,
    BasicBlock* bb) {
  JIT_CHECK(index < successors_.size(), "Index out of range");
  BasicBlock* old_bb = successors_[index];
  if (old_bb == bb) {
    return;
  }

  const IncomingEdge old_edge =
      old_bb->incomingEdge(old_successor_incoming_slot);
  JIT_CHECK(
      old_edge.predecessor() == this && old_edge.outgoingSlot() == index,
      "Incoming slot does not match successor edge");
  old_bb->erasePredecessor(old_successor_incoming_slot);
  successors_[index] = bb;
  bb->addPredecessor(this);
}

void BasicBlock::popSuccessor() {
  JIT_THROW_IF(
      successors_.empty(), "No successor to remove from block {}", id_);
  const IncomingEdge edge = outgoingEdge(successors_.size() - 1);
  edge.successor()->erasePredecessor(edge.incomingSlot());
  successors_.pop_back();
}

const std::vector<BasicBlock*>& BasicBlock::successors() const {
  return successors_;
}

IncomingEdge BasicBlock::outgoingEdge(size_t index) const {
  JIT_THROW_IF(
      index >= successors_.size(),
      "Successor index {} out of range for block {}, has {} successors",
      index,
      id_,
      successors_.size());
  BasicBlock* const successor = successors_[index];
  size_t occurrence = 0;
  for (size_t earlier = 0; earlier < index; ++earlier) {
    occurrence += successors_[earlier] == successor;
  }
  for (size_t incoming_slot = 0; incoming_slot < successor->numPredecessors();
       ++incoming_slot) {
    if (successor->predecessor(incoming_slot) != this) {
      continue;
    }
    if (occurrence == 0) {
      return successor->incomingEdge(incoming_slot);
    }
    --occurrence;
  }
  JIT_ABORT("Successor has no matching predecessor edge");
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

IncomingEdge BasicBlock::incomingEdge(size_t index) const {
  JIT_THROW_IF(
      index >= predecessors_.size(),
      "Predecessor index {} out of range for block {}, has {} predecessors",
      index,
      id_,
      predecessors_.size());
  return IncomingEdge{const_cast<BasicBlock*>(this), index};
}

size_t BasicBlock::addPredecessor(BasicBlock* predecessor) {
  const size_t incoming_slot = predecessors_.size();
  foreachPhiInstr([&](Instruction* instr) {
    JIT_CHECK(
        instr->numPhiInputs() == incoming_slot,
        "Phi input slots do not match predecessors");
    instr->setNumInputs(incoming_slot + 1);
  });
  predecessors_.push_back(predecessor);
  return incoming_slot;
}

void BasicBlock::replacePredecessor(
    size_t predecessor_index,
    BasicBlock* replacement) {
  BasicBlock* predecessor = this->predecessor(predecessor_index);
  const IncomingEdge edge{this, predecessor_index};
  if (predecessor == replacement) {
    return;
  }

  predecessor->successors_.erase(
      predecessor->successors_.begin() + edge.outgoingSlot());
  replacement->appendSuccessor(this);
  predecessors_[predecessor_index] = replacement;
}

void BasicBlock::removePredecessor(size_t predecessor_index) {
  BasicBlock* predecessor = this->predecessor(predecessor_index);
  const IncomingEdge edge{this, predecessor_index};
  predecessor->successors_.erase(
      predecessor->successors_.begin() + edge.outgoingSlot());
  erasePredecessor(predecessor_index);
}

void BasicBlock::erasePredecessor(size_t index) {
  JIT_THROW_IF(
      index >= predecessors_.size(),
      "Predecessor index {} out of range for block {}, has {} predecessors",
      index,
      id_,
      predecessors_.size());
  foreachPhiInstr([&](Instruction* instr) { instr->erasePhiInput(index); });

  predecessors_.erase(predecessors_.begin() + index);
}

void BasicBlock::compactPredecessors(const std::vector<bool>& keep) {
  JIT_CHECK(
      keep.size() == predecessors_.size(),
      "Predecessor keep mask size mismatch");

  for (size_t input = predecessors_.size(); input > 0; --input) {
    const size_t incoming_slot = input - 1;
    if (keep[incoming_slot]) {
      continue;
    }
    const IncomingEdge edge = incomingEdge(incoming_slot);
    BasicBlock* predecessor = edge.predecessor();
    predecessor->successors_.erase(
        predecessor->successors_.begin() + edge.outgoingSlot());
  }

  size_t output = 0;
  for (size_t input = 0; input < predecessors_.size(); ++input) {
    BasicBlock* predecessor = predecessors_[input];
    if (keep[input]) {
      predecessors_[output++] = predecessor;
    }
  }

  if (output == predecessors_.size()) {
    return;
  }
  foreachPhiInstr([&](Instruction* instr) { instr->compactPhiInputs(keep); });
  predecessors_.resize(output);
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

BasicBlock* BasicBlock::insertBasicBlockBetween(
    BasicBlock* block,
    size_t block_incoming_slot) {
  const IncomingEdge edge = block->incomingEdge(block_incoming_slot);
  JIT_CHECK(
      edge.predecessor() == this,
      "Incoming slot does not match successor edge");
  const size_t outgoing_slot = edge.outgoingSlot();

  auto new_block = func_->allocateBasicBlockAfter(this);
  block->replacePredecessor(block_incoming_slot, new_block);
  new_block->addPredecessor(this);
  successors_.insert(successors_.begin() + outgoing_slot, new_block);

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

  // Move outgoing edges to the new block in their existing order.
  while (!successors_.empty()) {
    const IncomingEdge edge = outgoingEdge(0);
    edge.successor()->replacePredecessor(edge.incomingSlot(), second_block);
  }

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
