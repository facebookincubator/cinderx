// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/instruction.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"

#include <array>
#include <utility>

namespace cinderx::jit::lir {

Instruction::Instruction(
    BasicBlock* basic_block,
    Opcode opcode,
    const hir::Instr* origin)
    : id_(basic_block->function()->allocateId()),
      opcode_(opcode),
      output_(this),
      basic_block_(basic_block),
      origin_(origin) {}

Instruction::Instruction(
    BasicBlock* bb,
    Instruction* instr,
    const hir::Instr* origin)
    : id_(bb->function()->allocateId()),
      opcode_(instr->opcode_),
      cond_(instr->cond_),
      output_(this, &instr->output_),
      basic_block_(bb),
      origin_(origin) {}

int Instruction::id() const {
  return id_;
}

void Instruction::setId(int id) {
  id_ = id;
}

Operand* Instruction::output() {
  return &output_;
}

const Operand* Instruction::output() const {
  return &output_;
}

const hir::Instr* Instruction::origin() const {
  return origin_;
}

size_t Instruction::getNumInputs() const {
  return inputs_.size();
}

size_t Instruction::numPhiInputs() const {
  JIT_CHECK(isPhi(), "Instruction is not a phi");
  JIT_CHECK(inputs_.size() % 2 == 0, "Phi inputs must be label/value pairs");
  return inputs_.size() / 2;
}

BasicBlock* Instruction::phiPredecessor(size_t index) const {
  JIT_CHECK(index < numPhiInputs(), "Phi input index out of range");
  const Operand* label = getInput(index * 2);
  return label == nullptr ? basic_block_->predecessor(index)
                          : label->getBasicBlock();
}

Operand* Instruction::phiInput(size_t index) {
  JIT_CHECK(index < numPhiInputs(), "Phi input index out of range");
  return getInput(index * 2 + 1);
}

const Operand* Instruction::phiInput(size_t index) const {
  JIT_CHECK(index < numPhiInputs(), "Phi input index out of range");
  return getInput(index * 2 + 1);
}

void Instruction::addPhiInput(IncomingEdge edge, Instruction* value) {
  JIT_CHECK(value != nullptr, "Phi input value is null");
  addPhiInput(edge, std::make_unique<Operand>(value, Operand::kLinked));
}

void Instruction::addPhiInput(
    IncomingEdge edge,
    std::unique_ptr<Operand> value) {
  JIT_CHECK(isPhi(), "Instruction is not a phi");
  JIT_CHECK(value != nullptr, "Phi input value is null");
  JIT_CHECK(edge.successor() == basic_block_, "Edge belongs to another block");

  const size_t incoming_slot = edge.incomingSlot();
  const size_t num_predecessors = basic_block_->numPredecessors();
  JIT_CHECK(incoming_slot < num_predecessors, "Incoming slot out of range");

  if (inputs_.empty()) {
    setNumInputs(num_predecessors * 2);
  }
  JIT_CHECK(
      numPhiInputs() == num_predecessors,
      "Phi input slots do not match predecessors");

  const size_t label_index = incoming_slot * 2;
  const size_t value_index = label_index + 1;
  JIT_CHECK(
      inputs_[label_index] == nullptr && inputs_[value_index] == nullptr,
      "Phi input already set");

  auto label = std::make_unique<Operand>(this);
  label->setBasicBlock(edge.predecessor());
  setInput(label_index, std::move(label));
  setInput(value_index, std::move(value));
}

void Instruction::setNumInputs(size_t n) {
  inputs_.resize(n);
}

size_t Instruction::getNumOutputs() const {
  return output_.type() == Operand::kNone ? 0 : 1;
}

Operand* Instruction::getInput(size_t i) {
  return inputs_.at(i).get();
}

const Operand* Instruction::getInput(size_t i) const {
  return inputs_.at(i).get();
}

Operand* Instruction::allocateImmediateInput(uint64_t n, DataType data_type) {
  auto operand = std::make_unique<Operand>(this, data_type, Operand::kImm, n);
  auto opnd = operand.get();
  inputs_.push_back(std::move(operand));

  return opnd;
}

Operand* Instruction::allocateFPImmediateInput(double n) {
  auto operand = std::make_unique<Operand>(this, Operand::kImm, n);
  auto opnd = operand.get();
  inputs_.push_back(std::move(operand));

  return opnd;
}

Operand* Instruction::allocateLinkedInput(Instruction* def_instr) {
  auto operand = std::make_unique<Operand>(this, def_instr, Operand::kLinked);
  Operand* opnd = operand.get();
  inputs_.push_back(std::move(operand));
  return opnd;
}

Operand* Instruction::allocatePhyRegisterInput(PhyLocation loc) {
  return allocateOperand(&Operand::setPhyRegister, loc);
}

Operand* Instruction::allocateStackInput(PhyLocation stack) {
  return allocateOperand(&Operand::setStackSlot, stack);
}

Operand* Instruction::allocatePhyRegOrStackInput(PhyLocation loc) {
  return allocateOperand(&Operand::setPhyRegOrStackSlot, loc);
}

Operand* Instruction::allocateAddressInput(void* address) {
  return allocateOperand(&Operand::setMemoryAddress, address);
}

Operand* Instruction::allocateLabelInput(BasicBlock* block) {
  return allocateOperand(&Operand::setBasicBlock, block);
}

Operand* Instruction::allocateAsmLabelInput(const asmjit::Label& label) {
  return allocateOperand(&Operand::setAsmLabel, label);
}

void Instruction::setBasicBlock(BasicBlock* bb) {
  basic_block_ = bb;
}

BasicBlock* Instruction::basicBlock() {
  return basic_block_;
}

const BasicBlock* Instruction::basicBlock() const {
  return basic_block_;
}

Opcode Instruction::opcode() const {
  return opcode_;
}

void Instruction::setOpcode(Opcode opcode) {
  opcode_ = opcode;
}

Condition Instruction::condition() const {
  JIT_DCHECK(
      carriesCondition(opcode_),
      "{} carries no condition",
      lir::opname(opcode_));
  return cond_;
}

void Instruction::setCondition(Condition cond) {
  JIT_DCHECK(
      carriesCondition(opcode_),
      "{} carries no condition",
      lir::opname(opcode_));
  cond_ = cond;
}

std::string_view Instruction::opname() const {
  // BranchCC and Compare print under the per-condition names the opcodes used
  // to have, so LIR dumps read the same as before the condition became a field.
  switch (opcode_) {
    case Opcode::kBranchCC:
      return branchCCName(cond_);
    case Opcode::kCompare:
      return compareName(cond_);
    default:
      break;
  }
  return lir::opname(opcode());
}

void Instruction::setInput(size_t i, std::unique_ptr<Operand> input) {
  inputs_.at(i) = std::move(input);
  inputs_[i]->assignToInstr(this);
}

std::unique_ptr<Operand> Instruction::removeInput(size_t index) {
  auto operand = releaseInput(index);
  inputs_.erase(inputs_.begin() + index);
  return operand;
}

std::unique_ptr<Operand> Instruction::releaseInput(size_t index) {
  auto& operand = inputs_.at(index);
  operand->releaseFromInstr();
  return std::move(inputs_.at(index));
}

Operand* Instruction::appendInput(std::unique_ptr<Operand> operand) {
  Operand* operand_ptr = operand.get();
  // Use setInput() to call assignToInstr().
  inputs_.emplace_back();
  setInput(getNumInputs() - 1, std::move(operand));
  return operand_ptr;
}

Operand* Instruction::prependInput(std::unique_ptr<Operand> operand) {
  Operand* operand_ptr = operand.get();
  inputs_.insert(inputs_.begin(), nullptr);
  setInput(0, std::move(operand));
  return operand_ptr;
}

Operand* Instruction::getOperandByPredecessor(const BasicBlock* pred) {
  auto index = getOperandIndexByPredecessor(pred);
  return index == -1 ? nullptr : inputs_.at(index).get();
}

int Instruction::getOperandIndexByPredecessor(const BasicBlock* pred) const {
  JIT_DCHECK(opcode_ == Opcode::kPhi, "The current instruction must be Phi.");
  size_t num_inputs = getNumInputs();
  for (size_t i = 0; i < num_inputs; i += 2) {
    const Operand* label = getInput(i);
    if (label != nullptr && label->getBasicBlock() == pred) {
      return i + 1;
    }
  }
  return -1;
}

const Operand* Instruction::getOperandByPredecessor(
    const BasicBlock* pred) const {
  return const_cast<Instruction*>(this)->getOperandByPredecessor(pred);
}

bool Instruction::getOutputPhyRegUse() const {
  return outputMustBeRegister(opcode_);
}

bool Instruction::getInputPhyRegUse(size_t i) const {
  // If the output of a move/store instruction is a memory location, then its
  // input needs to be a physical register. Otherwise we might generate a
  // mem->mem move, which we can't safely handle for all bit widths in codegen
  // (since push/pop aren't available for all bit widths).
  if ((isMove() || isMoveRelaxed() || isStore()) && output_.isInd()) {
    return true;
  }

  return inputMustBeRegister(opcode_, i);
}

bool Instruction::inputsLiveAcross() const {
  return lir::inputsLiveAcross(opcode_);
}

} // namespace cinderx::jit::lir
