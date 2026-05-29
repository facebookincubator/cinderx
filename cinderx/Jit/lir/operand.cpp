// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/operand.h"

#include "cinderx/Jit/lir/arch.h"
#include "cinderx/Jit/lir/instruction.h"

namespace jit::lir {

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

Operand::Operand(const Operand& other)
    : parent_instr_{other.parent_instr_},
      def_opnd_{other.def_opnd_},
      last_use_{other.last_use_},
      type_{other.type_},
      data_type_{other.data_type_} {}

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
  value_ = bit_cast<uint64_t>(data);
}

Operand::Operand(Instruction* def_instr, LinkedTag) {
  if (def_instr != nullptr) {
    def_opnd_ = def_instr->output();
  }
}

Operand::Operand(Instruction* parent, Instruction* def_instr, LinkedTag)
    : parent_instr_{parent} {
  if (def_instr != nullptr) {
    def_opnd_ = def_instr->output();
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
  return getPhyRegister().is_fp_register();
}

bool Operand::isLastUse() const {
  return last_use_;
}

void Operand::setLastUse() {
  last_use_ = true;
}

uint64_t Operand::getConstant() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->getConstant();
  }
  return std::get<uint64_t>(value_);
}

void Operand::setConstant(uint64_t n, DataType data_type) {
  type_ = kImm;
  value_ = n;
  data_type_ = data_type;
}

double Operand::getFPConstant() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->getFPConstant();
  }
  uint64_t value = std::get<uint64_t>(value_);
  return bit_cast<double>(value);
}

void Operand::setFPConstant(double n) {
  type_ = kImm;
  data_type_ = kDouble;
  value_ = bit_cast<uint64_t>(n);
}

PhyLocation Operand::getPhyRegister() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->getPhyRegister();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->getStackSlot();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->getPhyRegOrStackSlot();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->getMemoryAddress();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->getMemoryIndirect();
  }
  JIT_CHECK(
      type_ == kInd,
      "Trying to treat operand [type={},val={:#x}] as a memory indirect",
      type_,
      rawValue());
  return std::get<std::unique_ptr<MemoryIndirect>>(value_).get();
}

BasicBlock* Operand::getBasicBlock() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->getBasicBlock();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->getAsmLabel();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->hasAsmLabel();
  }
  return type_ == kLabel && std::holds_alternative<asmjit::Label>(value_);
}

uint64_t Operand::getConstantOrAddress() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->getConstantOrAddress();
  }
  if (const uint64_t* v = std::get_if<uint64_t>(&value_)) {
    return *v;
  }
  return reinterpret_cast<uint64_t>(getMemoryAddress());
}

const Operand* Operand::getDefine() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_;
  }
  return this;
}

Operand* Operand::getDefine() {
  if (def_opnd_ != nullptr) {
    return def_opnd_;
  }
  return this;
}

DataType Operand::dataType() const {
  if (def_opnd_ != nullptr) {
    return def_opnd_->dataType();
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
  if (def_opnd_ != nullptr) {
    return def_opnd_->type();
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
  return def_opnd_ != nullptr;
}

Operand* Operand::getLinkedOperand() {
  return def_opnd_;
}

const Operand* Operand::getLinkedOperand() const {
  return def_opnd_;
}

Instruction* Operand::getLinkedInstr() {
  return def_opnd_->instr();
}

const Instruction* Operand::getLinkedInstr() const {
  return def_opnd_->instr();
}

void Operand::setLinkedInstr(Instruction* def) {
  def_opnd_ = def->output();
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
  }

  JIT_ABORT("Unknown operand value type, has index {}", value_.index());
}

} // namespace jit::lir
