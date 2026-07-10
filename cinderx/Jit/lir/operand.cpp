// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/operand.h"

#include "cinderx/Jit/lir/arch.h"
#include "cinderx/Jit/lir/instruction.h"

#include <bit>

namespace cinderx::jit::lir {

MemoryIndirect::MemoryIndirect(Instruction* parent) : parent_(parent) {}

MemoryIndirect::~MemoryIndirect() = default;

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

Operand* MemoryIndirect::getBaseRegOperand() const {
  return base_reg_.get();
}

Operand* MemoryIndirect::getIndexRegOperand() const {
  return index_reg_.get();
}

std::unique_ptr<Operand> MemoryIndirect::releaseBaseRegOperand() {
  return std::move(base_reg_);
}

std::unique_ptr<Operand> MemoryIndirect::releaseIndexRegOperand() {
  return std::move(index_reg_);
}

uint8_t MemoryIndirect::getMultiplier() const {
  return multiplier_;
}

int32_t MemoryIndirect::getOffset() const {
  return offset_;
}

void MemoryIndirect::setBaseIndex(
    std::unique_ptr<Operand>& base_index_opnd,
    Instruction* base_index) {
  if (base_index != nullptr) {
    base_index_opnd =
        std::make_unique<Operand>(parent_, base_index, Operand::kLinked);
  } else {
    base_index_opnd.reset();
  }
}
void MemoryIndirect::setBaseIndex(
    std::unique_ptr<Operand>& base_index_opnd,
    PhyLocation base_index) {
  if (base_index != PhyLocation::REG_INVALID) {
    base_index_opnd = std::make_unique<Operand>(parent_);
    base_index_opnd->setPhyRegister(base_index);
  } else {
    base_index_opnd.reset();
  }
}

void MemoryIndirect::setBaseIndex(
    std::unique_ptr<Operand>& base_index_opnd,
    std::variant<Instruction*, PhyLocation> base_index) {
  if (Instruction** instrp = std::get_if<Instruction*>(&base_index)) {
    setBaseIndex(base_index_opnd, *instrp);
  } else {
    setBaseIndex(base_index_opnd, std::get<PhyLocation>(base_index));
  }
}

Operand::Operand(Instruction* parent) : parent_instr_{parent} {}

// Only copies simple fields (type and data type) from operand.
// The value_ field is not copied.
Operand::Operand(Instruction* parent, Operand* operand)
    : parent_instr_{parent},
      type_{operand->type_},
      data_type_{operand->data_type_} {}

Operand::Operand(
    Instruction* parent,
    DataType data_type,
    Operand::Type type,
    uint64_t data)
    : parent_instr_{parent}, type_{type}, data_type_{data_type} {
  value_ = data;
}

Operand::Operand(Instruction* parent, Operand::Type type, double data)
    : parent_instr_{parent}, type_{type}, data_type_{kDouble} {
  value_ = std::bit_cast<uint64_t>(data);
}

Operand::Operand(Instruction* def_instr, LinkedTag) {
  if (def_instr != nullptr) {
    value_ = def_instr->output();
  }
}

Operand::Operand(Instruction* parent, Instruction* def_instr, LinkedTag)
    : parent_instr_{parent} {
  if (def_instr != nullptr) {
    value_ = def_instr->output();
  }
}

size_t Operand::sizeInBits() const {
  return bitSize(dataType());
}

Instruction* Operand::instr() {
  return parent_instr_;
}

const Instruction* Operand::instr() const {
  return parent_instr_;
}

void Operand::assignToInstr(Instruction* instr) {
  parent_instr_ = instr;
}

void Operand::releaseFromInstr() {
  parent_instr_ = nullptr;
}

bool Operand::isFp() const {
  return dataType() == kDouble;
}

bool Operand::isVecD() const {
  return getPhyRegister().isFpRegister();
}

bool Operand::isLastUse() const {
  return last_use_;
}

void Operand::setLastUse() {
  last_use_ = true;
}

uint64_t Operand::getConstant() const {
  if (const Operand* def = linkedDef()) {
    return def->getConstant();
  }
  return std::get<uint64_t>(value_);
}

void Operand::setConstant(uint64_t n, DataType data_type) {
  type_ = kImm;
  value_ = n;
  data_type_ = data_type;
}

double Operand::getFPConstant() const {
  if (const Operand* def = linkedDef()) {
    return def->getFPConstant();
  }
  uint64_t value = std::get<uint64_t>(value_);
  return std::bit_cast<double>(value);
}

void Operand::setFPConstant(double n) {
  type_ = kImm;
  data_type_ = kDouble;
  value_ = std::bit_cast<uint64_t>(n);
}

PhyLocation Operand::getPhyRegister() const {
  if (const Operand* def = linkedDef()) {
    return def->getPhyRegister();
  }
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
  if (const Operand* def = linkedDef()) {
    return def->getStackSlot();
  }
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
  if (const Operand* def = linkedDef()) {
    return def->getPhyRegOrStackSlot();
  }
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
  if (const Operand* def = linkedDef()) {
    return def->getMemoryAddress();
  }
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
  if (const Operand* def = linkedDef()) {
    return def->getMemoryIndirect();
  }
  JIT_CHECK(
      type_ == kInd,
      "Trying to treat operand [type={},val={:#x}] as a memory indirect",
      type_,
      rawValue());
  return std::get<std::unique_ptr<MemoryIndirect>>(value_).get();
}

BasicBlock* Operand::getBasicBlock() const {
  if (const Operand* def = linkedDef()) {
    return def->getBasicBlock();
  }
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

asmjit::Label Operand::getAsmLabel() const {
  if (const Operand* def = linkedDef()) {
    return def->getAsmLabel();
  }
  JIT_CHECK(
      type_ == kLabel,
      "Trying to treat operand [type={}] as an asmjit label",
      type_);
  return std::get<asmjit::Label>(value_);
}

void Operand::setAsmLabel(const asmjit::Label& label) {
  type_ = kLabel;
  data_type_ = kObject;
  value_ = label;
}

bool Operand::hasAsmLabel() const {
  if (const Operand* def = linkedDef()) {
    return def->hasAsmLabel();
  }
  return type_ == kLabel && std::holds_alternative<asmjit::Label>(value_);
}

uint64_t Operand::getConstantOrAddress() const {
  if (const Operand* def = linkedDef()) {
    return def->getConstantOrAddress();
  }
  if (const uint64_t* v = std::get_if<uint64_t>(&value_)) {
    return *v;
  }
  return reinterpret_cast<uint64_t>(getMemoryAddress());
}

const Operand* Operand::getDefine() const {
  if (const Operand* def = linkedDef()) {
    return def;
  }
  return this;
}

Operand* Operand::getDefine() {
  if (Operand* def = linkedDef()) {
    return def;
  }
  return this;
}

DataType Operand::dataType() const {
  if (const Operand* def = linkedDef()) {
    return def->dataType();
  }
  return data_type_;
}

void Operand::setDataType(DataType data_type) {
  data_type_ = data_type;
  if (PhyLocation* loc_ptr = std::get_if<PhyLocation>(&value_)) {
    loc_ptr->bitSize = bitSize(data_type);
  }
}

Operand::Type Operand::type() const {
  if (const Operand* def = linkedDef()) {
    return def->type();
  }
  return type_;
}

void Operand::setNone() {
  type_ = kNone;
}

void Operand::setVirtualRegister() {
  type_ = kVreg;
}

bool Operand::isLinked() const {
  return std::holds_alternative<Operand*>(value_);
}

Operand* Operand::getLinkedOperand() {
  return std::get<Operand*>(value_);
}

const Operand* Operand::getLinkedOperand() const {
  return std::get<Operand*>(value_);
}

Instruction* Operand::getLinkedInstr() {
  return std::get<Operand*>(value_)->instr();
}

const Instruction* Operand::getLinkedInstr() const {
  return std::get<Operand*>(value_)->instr();
}

void Operand::setLinkedInstr(Instruction* def) {
  value_ = def->output();
}

Operand* Operand::linkedDef() {
  if (Operand** def = std::get_if<Operand*>(&value_)) {
    return *def;
  }
  return nullptr;
}

const Operand* Operand::linkedDef() const {
  if (Operand* const* def = std::get_if<Operand*>(&value_)) {
    return *def;
  }
  return nullptr;
}

uint64_t Operand::rawValue() const {
  if (const uint64_t* ptr = std::get_if<uint64_t>(&value_)) {
    return *ptr;
  } else if (void* const* void_ptr = std::get_if<void*>(&value_)) {
    return reinterpret_cast<uint64_t>(*void_ptr);
  } else if (BasicBlock* const* bb_ptr = std::get_if<BasicBlock*>(&value_)) {
    return reinterpret_cast<uint64_t>(*bb_ptr);
  } else if (
      const std::unique_ptr<MemoryIndirect>* mem_ptr =
          std::get_if<std::unique_ptr<MemoryIndirect>>(&value_)) {
    return reinterpret_cast<uint64_t>(mem_ptr->get());
  } else if (const PhyLocation* phy_ptr = std::get_if<PhyLocation>(&value_)) {
    return static_cast<uint64_t>(phy_ptr->loc);
  } else if (Operand* const* def = std::get_if<Operand*>(&value_)) {
    return reinterpret_cast<uint64_t>(*def);
  }

  JIT_ABORT("Unknown operand value type, has index {}", value_.index());
}

} // namespace cinderx::jit::lir
