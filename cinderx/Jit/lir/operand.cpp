// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/operand.h"

#include "cinderx/Jit/lir/arch.h"
#include "cinderx/Jit/lir/instruction.h"

namespace jit::lir {

OperandBase::OperandBase(Instruction* parent) : parent_instr_{parent} {}

OperandBase::OperandBase(const OperandBase& ob)
    : parent_instr_{ob.parent_instr_}, last_use_{ob.last_use_} {}

size_t OperandBase::sizeInBits() const {
  return bitSize(dataType());
}

Instruction* OperandBase::instr() {
  return parent_instr_;
}

const Instruction* OperandBase::instr() const {
  return parent_instr_;
}

void OperandBase::assignToInstr(Instruction* instr) {
  parent_instr_ = instr;
}

void OperandBase::releaseFromInstr() {
  parent_instr_ = nullptr;
}

bool OperandBase::isFp() const {
  return dataType() == kDouble;
}

bool OperandBase::isVecD() const {
  return getPhyRegister().is_fp_register();
}

bool OperandBase::isLastUse() const {
  return last_use_;
}

void OperandBase::setLastUse() {
  last_use_ = true;
}

MemoryIndirect::MemoryIndirect(Instruction* parent) : parent_(parent) {}

void MemoryIndirect::setMemoryIndirect(Instruction* base, int32_t offset) {
  setMemoryIndirect(base, nullptr /* index */, 0, offset);
}

void MemoryIndirect::setMemoryIndirect(PhyLocation base, int32_t offset) {
  setMemoryIndirect(base, PhyLocation::REG_INVALID, 0, offset);
}

void MemoryIndirect::setMemoryIndirect(
    PhyLocation base,
    PhyLocation index_reg,
    uint8_t multiplier) {
  setMemoryIndirect(base, index_reg, multiplier, 0);
}

void MemoryIndirect::setMemoryIndirect(
    std::variant<Instruction*, PhyLocation> base,
    std::variant<Instruction*, PhyLocation> index,
    uint8_t multiplier,
    int32_t offset) {
  setBaseIndex(base_reg_, base);
  setBaseIndex(index_reg_, index);
  multiplier_ = multiplier;
  offset_ = offset;
}

OperandBase* MemoryIndirect::getBaseRegOperand() const {
  return base_reg_.get();
}

OperandBase* MemoryIndirect::getIndexRegOperand() const {
  return index_reg_.get();
}

uint8_t MemoryIndirect::getMultipiler() const {
  return multiplier_;
}

int32_t MemoryIndirect::getOffset() const {
  return offset_;
}

void MemoryIndirect::setBaseIndex(
    std::unique_ptr<OperandBase>& base_index_opnd,
    Instruction* base_index) {
  if (base_index != nullptr) {
    base_index_opnd = std::make_unique<LinkedOperand>(parent_, base_index);
  } else {
    base_index_opnd.reset();
  }
}
void MemoryIndirect::setBaseIndex(
    std::unique_ptr<OperandBase>& base_index_opnd,
    PhyLocation base_index) {
  if (base_index != PhyLocation::REG_INVALID) {
    auto operand = std::make_unique<Operand>(parent_);
    operand->setPhyRegister(base_index);
    base_index_opnd = std::move(operand);
  } else {
    base_index_opnd.reset();
  }
}

void MemoryIndirect::setBaseIndex(
    std::unique_ptr<OperandBase>& base_index_opnd,
    std::variant<Instruction*, PhyLocation> base_index) {
  if (Instruction** instrp = std::get_if<Instruction*>(&base_index)) {
    setBaseIndex(base_index_opnd, *instrp);
  } else {
    setBaseIndex(base_index_opnd, std::get<PhyLocation>(base_index));
  }
}

Operand::Operand(Instruction* parent) : OperandBase{parent} {}

// Only copies simple fields (type and data type) from operand.
// The value_ field is not copied.
Operand::Operand(Instruction* parent, Operand* operand)
    : OperandBase(parent),
      type_(operand->type_),
      data_type_(operand->data_type_) {}

Operand::Operand(
    Instruction* parent,
    DataType data_type,
    Operand::Type type,
    uint64_t data)
    : OperandBase(parent), type_(type), data_type_(data_type) {
  value_ = data;
}

Operand::Operand(Instruction* parent, Operand::Type type, double data)
    : OperandBase(parent), type_(type), data_type_(kDouble) {
  value_ = bit_cast<uint64_t>(data);
}

uint64_t Operand::getConstant() const {
  return std::get<uint64_t>(value_);
}

void Operand::setConstant(uint64_t n, DataType data_type) {
  type_ = kImm;
  value_ = n;
  data_type_ = data_type;
}

double Operand::getFPConstant() const {
  auto value = std::get<uint64_t>(value_);
  return bit_cast<double>(value);
}

void Operand::setFPConstant(double n) {
  type_ = kImm;
  data_type_ = kDouble;
  value_ = bit_cast<uint64_t>(n);
}

PhyLocation Operand::getPhyRegister() const {
  JIT_CHECK(
      type_ == kReg,
      "Trying to treat operand [type={},val={:#x}] as a physical register",
      type_,
      rawValue());
  return std::get<PhyLocation>(value_);
}

void Operand::setPhyRegister(PhyLocation reg) {
  type_ = kReg;
  value_ = reg;
}

PhyLocation Operand::getStackSlot() const {
  JIT_CHECK(
      type_ == kStack,
      "Trying to treat operand [type={},val={:#x}] as a stack slot",
      type_,
      rawValue());
  return std::get<PhyLocation>(value_);
}

void Operand::setStackSlot(PhyLocation slot) {
  type_ = kStack;
  value_ = slot;
}

PhyLocation Operand::getPhyRegOrStackSlot() const {
  switch (type_) {
    case kReg:
      return getPhyRegister();
    case kStack:
      return getStackSlot();
    default:
      JIT_ABORT(
          "Trying to treat operand [type={},val={:#x} as a physical register "
          "or a stack slot",
          type_,
          rawValue());
  }
  return -1;
}

void Operand::setPhyRegOrStackSlot(PhyLocation loc) {
  if (loc.loc < 0) {
    setStackSlot(loc);
  } else {
    setPhyRegister(loc);
  }
}

void* Operand::getMemoryAddress() const {
  JIT_CHECK(
      type_ == kMem,
      "Trying to treat operand [type={},val={:#x}] as a memory address",
      type_,
      rawValue());
  return std::get<void*>(value_);
}

void Operand::setMemoryAddress(void* addr) {
  type_ = kMem;
  value_ = addr;
}

MemoryIndirect* Operand::getMemoryIndirect() const {
  JIT_CHECK(
      type_ == kInd,
      "Trying to treat operand [type={},val={:#x}] as a memory indirect",
      type_,
      rawValue());
  return std::get<std::unique_ptr<MemoryIndirect>>(value_).get();
}

BasicBlock* Operand::getBasicBlock() const {
  JIT_CHECK(
      type_ == kLabel,
      "Trying to treat operand [type={},val={:#x}] as a basic block address",
      type_,
      rawValue());
  return std::get<BasicBlock*>(value_);
}

void Operand::setBasicBlock(BasicBlock* block) {
  type_ = kLabel;
  data_type_ = kObject;
  value_ = block;
}

uint64_t Operand::getConstantOrAddress() const {
  if (auto v = std::get_if<uint64_t>(&value_)) {
    return *v;
  }
  return reinterpret_cast<uint64_t>(getMemoryAddress());
}

const Operand* Operand::getDefine() const {
  return this;
}

Operand* Operand::getDefine() {
  return this;
}

DataType Operand::dataType() const {
  return data_type_;
}

void Operand::setDataType(DataType data_type) {
  data_type_ = data_type;
  if (auto loc_ptr = std::get_if<PhyLocation>(&value_)) {
    loc_ptr->bitSize = bitSize(data_type);
  }
}

Operand::Type Operand::type() const {
  return type_;
}

void Operand::setNone() {
  type_ = kNone;
}

void Operand::setVirtualRegister() {
  type_ = kVreg;
}

bool Operand::isLinked() const {
  return false;
}

uint64_t Operand::rawValue() const {
  if (const auto ptr = std::get_if<uint64_t>(&value_)) {
    return *ptr;
  } else if (const auto void_ptr = std::get_if<void*>(&value_)) {
    return reinterpret_cast<uint64_t>(*void_ptr);
  } else if (const auto bb_ptr = std::get_if<BasicBlock*>(&value_)) {
    return reinterpret_cast<uint64_t>(*bb_ptr);
  } else if (
      const auto mem_ptr =
          std::get_if<std::unique_ptr<MemoryIndirect>>(&value_)) {
    return reinterpret_cast<uint64_t>(mem_ptr->get());
  } else if (const auto phy_ptr = std::get_if<PhyLocation>(&value_)) {
    return static_cast<uint64_t>(phy_ptr->loc);
  }

  JIT_ABORT("Unknown operand value type, has index {}", value_.index());
}

LinkedOperand::LinkedOperand(Instruction* def_instr) {
  def_opnd_ = def_instr->output();
}

LinkedOperand::LinkedOperand(Instruction* parent, Instruction* def_instr)
    : LinkedOperand{def_instr} {
  assignToInstr(parent);
}

Operand* LinkedOperand::getLinkedOperand() {
  return def_opnd_;
}

const Operand* LinkedOperand::getLinkedOperand() const {
  return def_opnd_;
}

Instruction* LinkedOperand::getLinkedInstr() {
  return def_opnd_->instr();
}

const Instruction* LinkedOperand::getLinkedInstr() const {
  return def_opnd_->instr();
}

void LinkedOperand::setLinkedInstr(Instruction* def) {
  def_opnd_ = def->output();
}

uint64_t LinkedOperand::getConstant() const {
  return def_opnd_->getConstant();
}

double LinkedOperand::getFPConstant() const {
  return def_opnd_->getFPConstant();
}

PhyLocation LinkedOperand::getPhyRegister() const {
  return def_opnd_->getPhyRegister();
}

PhyLocation LinkedOperand::getStackSlot() const {
  return def_opnd_->getStackSlot();
}

PhyLocation LinkedOperand::getPhyRegOrStackSlot() const {
  return def_opnd_->getPhyRegOrStackSlot();
}

void* LinkedOperand::getMemoryAddress() const {
  return def_opnd_->getMemoryAddress();
}

MemoryIndirect* LinkedOperand::getMemoryIndirect() const {
  return def_opnd_->getMemoryIndirect();
}

BasicBlock* LinkedOperand::getBasicBlock() const {
  return def_opnd_->getBasicBlock();
}

uint64_t LinkedOperand::getConstantOrAddress() const {
  return def_opnd_->getConstantOrAddress();
}

Operand* LinkedOperand::getDefine() {
  return def_opnd_;
}

const Operand* LinkedOperand::getDefine() const {
  return def_opnd_;
}

DataType LinkedOperand::dataType() const {
  return def_opnd_->dataType();
}

Operand::Type LinkedOperand::type() const {
  return def_opnd_->type();
}

bool LinkedOperand::isLinked() const {
  return true;
}

} // namespace jit::lir
