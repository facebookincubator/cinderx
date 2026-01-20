// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/instruction.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"

#include <array>
#include <utility>

namespace jit::lir {

#define COUNT_INSTR(...) +1
constexpr size_t kNumOpcodes = FOREACH_INSTR_TYPE(COUNT_INSTR);
#undef COUNT_INSTR

constexpr std::array<std::string_view, kNumOpcodes> kOpcodeNames = {
#define INSTR_DECL_TYPE(v, ...) #v,
    FOREACH_INSTR_TYPE(INSTR_DECL_TYPE)
#undef INSTR_DECL_TYPE
};

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

void Instruction::setNumInputs(size_t n) {
  inputs_.resize(n);
}

size_t Instruction::getNumOutputs() const {
  return output_.type() == OperandBase::kNone ? 0 : 1;
}

OperandBase* Instruction::getInput(size_t i) {
  return inputs_.at(i).get();
}

const OperandBase* Instruction::getInput(size_t i) const {
  return inputs_.at(i).get();
}

Operand* Instruction::allocateImmediateInput(uint64_t n, DataType data_type) {
  auto operand =
      std::make_unique<Operand>(this, data_type, OperandBase::kImm, n);
  auto opnd = operand.get();
  inputs_.push_back(std::move(operand));

  return opnd;
}

Operand* Instruction::allocateFPImmediateInput(double n) {
  auto operand = std::make_unique<Operand>(this, OperandBase::kImm, n);
  auto opnd = operand.get();
  inputs_.push_back(std::move(operand));

  return opnd;
}

LinkedOperand* Instruction::allocateLinkedInput(Instruction* def_instr) {
  auto operand = std::make_unique<LinkedOperand>(this, def_instr);
  auto opnd = operand.get();
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

void Instruction::setbasicblock(BasicBlock* bb) {
  basic_block_ = bb;
}

BasicBlock* Instruction::basicblock() {
  return basic_block_;
}

const BasicBlock* Instruction::basicblock() const {
  return basic_block_;
}

Instruction::Opcode Instruction::opcode() const {
  return opcode_;
}

void Instruction::setOpcode(Opcode opcode) {
  opcode_ = opcode;
}

std::string_view Instruction::opname() const {
  return kOpcodeNames[opcode_];
}

void Instruction::setInput(size_t i, std::unique_ptr<OperandBase> input) {
  inputs_.at(i) = std::move(input);
  inputs_[i]->assignToInstr(this);
}

std::unique_ptr<OperandBase> Instruction::removeInput(size_t index) {
  auto operand = releaseInput(index);
  inputs_.erase(inputs_.begin() + index);
  return operand;
}

std::unique_ptr<OperandBase> Instruction::releaseInput(size_t index) {
  auto& operand = inputs_.at(index);
  operand->releaseFromInstr();
  return std::move(inputs_.at(index));
}

OperandBase* Instruction::appendInput(std::unique_ptr<OperandBase> operand) {
  auto operand_ptr = operand.get();
  // Use setInput() to call assignToInstr().
  inputs_.emplace_back();
  setInput(getNumInputs() - 1, std::move(operand));
  return operand_ptr;
}

OperandBase* Instruction::prependInput(std::unique_ptr<OperandBase> operand) {
  auto operand_ptr = operand.get();
  inputs_.insert(inputs_.begin(), nullptr);
  setInput(0, std::move(operand));
  return operand_ptr;
}

OperandBase* Instruction::getOperandByPredecessor(const BasicBlock* pred) {
  auto index = getOperandIndexByPredecessor(pred);
  return index == -1 ? nullptr : inputs_.at(index).get();
}

int Instruction::getOperandIndexByPredecessor(const BasicBlock* pred) const {
  JIT_DCHECK(opcode_ == kPhi, "The current instruction must be Phi.");
  size_t num_inputs = getNumInputs();
  for (size_t i = 0; i < num_inputs; i += 2) {
    if (getInput(i)->getBasicBlock() == pred) {
      return i + 1;
    }
  }
  return -1;
}

const OperandBase* Instruction::getOperandByPredecessor(
    const BasicBlock* pred) const {
  return const_cast<Instruction*>(this)->getOperandByPredecessor(pred);
}

bool Instruction::getOutputPhyRegUse() const {
  return InstrProperty::getProperties(opcode_).output_phy_use;
}

bool Instruction::getInputPhyRegUse(size_t i) const {
  // If the output of a move instruction is a memory location, then its input
  // needs to be a physical register. Otherwise we might generate a mem->mem
  // move, which we can't safely handle for all bit widths in codegen (since
  // push/pop aren't available for all bit widths).
  if (isMove() && output_.isInd()) {
    return true;
  }

  auto& uses = InstrProperty::getProperties(opcode_).input_phy_uses;
  if (i >= uses.size()) {
    return false;
  }

  return uses.at(i);
}

bool Instruction::inputsLiveAcross() const {
  return InstrProperty::getProperties(opcode_).inputs_live_across;
}

bool Instruction::isCompare() const {
  switch (opcode_) {
    case kEqual:
    case kNotEqual:
    case kGreaterThanSigned:
    case kLessThanSigned:
    case kGreaterThanEqualSigned:
    case kLessThanEqualSigned:
    case kGreaterThanUnsigned:
    case kLessThanUnsigned:
    case kGreaterThanEqualUnsigned:
    case kLessThanEqualUnsigned:
      return true;
    default:
      return false;
  }
}

bool Instruction::isBranchCC() const {
  switch (opcode_) {
    case kBranchC:
    case kBranchNC:
    case kBranchO:
    case kBranchNO:
    case kBranchS:
    case kBranchNS:
    case kBranchZ:
    case kBranchNZ:
    case kBranchA:
    case kBranchB:
    case kBranchBE:
    case kBranchAE:
    case kBranchL:
    case kBranchG:
    case kBranchLE:
    case kBranchGE:
    case kBranchE:
    case kBranchNE:
      return true;
    default:
      return false;
  }
}

bool Instruction::isAnyBranch() const {
  return (opcode_ == kCondBranch) || isBranchCC();
}

bool Instruction::isTerminator() const {
  switch (opcode_) {
    case kReturn:
      return true;
    default:
      return false;
  }
}

bool Instruction::isAnyYield() const {
  switch (opcode_) {
    case kYieldFrom:
    case kYieldFromHandleStopAsyncIteration:
    case kYieldFromSkipInitialSend:
    case kYieldInitial:
    case kYieldValue:
      return true;
    default:
      return false;
  }
}

#define CASE_FLIP(op1, op2) \
  case op1:                 \
    return op2;             \
  case op2:                 \
    return op1;

Instruction::Opcode Instruction::negateBranchCC(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(kBranchC, kBranchNC)
    CASE_FLIP(kBranchO, kBranchNO)
    CASE_FLIP(kBranchS, kBranchNS)
    CASE_FLIP(kBranchZ, kBranchNZ)
    CASE_FLIP(kBranchA, kBranchBE)
    CASE_FLIP(kBranchB, kBranchAE)
    CASE_FLIP(kBranchL, kBranchGE)
    CASE_FLIP(kBranchG, kBranchLE)
    CASE_FLIP(kBranchE, kBranchNE)
    default:
      JIT_ABORT("Not a conditional branch opcode: {}", kOpcodeNames[opcode]);
  }
}

Instruction::Opcode Instruction::flipBranchCCDirection(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(kBranchA, kBranchB)
    CASE_FLIP(kBranchAE, kBranchBE)
    CASE_FLIP(kBranchL, kBranchG)
    CASE_FLIP(kBranchLE, kBranchGE)
    default:
      JIT_ABORT(
          "Unable to flip branch condition for opcode: {}",
          kOpcodeNames[opcode]);
  }
}

Instruction::Opcode Instruction::flipComparisonDirection(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(kGreaterThanEqualSigned, kLessThanEqualSigned)
    CASE_FLIP(kGreaterThanEqualUnsigned, kLessThanEqualUnsigned)
    CASE_FLIP(kGreaterThanSigned, kLessThanSigned)
    CASE_FLIP(kGreaterThanUnsigned, kLessThanUnsigned)
    case kEqual:
      return kEqual;
    case kNotEqual:
      return kNotEqual;
    default:
      JIT_ABORT(
          "Unable to flip comparison direction for opcode: {}",
          kOpcodeNames[opcode]);
  }
}

#undef CASE_FLIP

Instruction::Opcode Instruction::compareToBranchCC(Opcode opcode) {
  switch (opcode) {
    case kEqual:
      return kBranchE;
    case kNotEqual:
      return kBranchNE;
    case kGreaterThanUnsigned:
      return kBranchA;
    case kLessThanUnsigned:
      return kBranchB;
    case kGreaterThanEqualUnsigned:
      return kBranchAE;
    case kLessThanEqualUnsigned:
      return kBranchBE;
    case kGreaterThanSigned:
      return kBranchG;
    case kLessThanSigned:
      return kBranchL;
    case kGreaterThanEqualSigned:
      return kBranchGE;
    case kLessThanEqualSigned:
      return kBranchLE;
    default:
      JIT_ABORT("Not a compare opcode.");
  }
}

InstrProperty::InstrInfo& InstrProperty::getProperties(
    Instruction::Opcode opcode) {
  return prop_map_.at(opcode);
}

#define BEGIN_INSTR_PROPERTY \
  std::vector<InstrProperty::InstrInfo> InstrProperty::prop_map_ = {
#define END_INSTR_PROPERTY \
  }                        \
  ;

#define PROPERTY(__t, __p...) {#__t, __p},

// clang-format off
// This table contains definitions of all the properties for each instruction type.
BEGIN_INSTR_PROPERTY
  FOREACH_INSTR_TYPE(PROPERTY)
END_INSTR_PROPERTY
// clang-format on

} // namespace jit::lir
