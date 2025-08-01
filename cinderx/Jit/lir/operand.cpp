// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/operand.h"

#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/x86_64.h"

namespace jit::lir {

size_t OperandBase::sizeInBits() const {
  return bitSize(dataType());
}

void Operand::addUse(LinkedOperand* use) {
  use->def_opnd_ = this;
}

void Operand::removeUse(LinkedOperand* use) {
  JIT_DCHECK(
      use->getLinkedOperand() == this,
      "Unable to remove use of another operand.");
  use->def_opnd_ = nullptr;
}

LinkedOperand::LinkedOperand(Instruction* parent, Instruction* def_instr)
    : OperandBase(parent), def_opnd_(nullptr) {
  if (def_instr != nullptr) {
    def_instr->output()->addUse(this);
  }
}

void LinkedOperand::setLinkedInstr(Instruction* def) {
  def_opnd_ = def->output();
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

} // namespace jit::lir
