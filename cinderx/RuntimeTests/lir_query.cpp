// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/RuntimeTests/lir_query.h"

#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/printer.h"

#include <fmt/format.h>

#include <memory>
#include <optional>

namespace cinderx::jit::lir {
namespace {

std::optional<uint64_t> immOrMemoryAddress(const Operand& operand) {
  if (operand.isImm()) {
    return operand.getConstant();
  }
  if (operand.isMem()) {
    return reinterpret_cast<uint64_t>(operand.getMemoryAddress());
  }
  return std::nullopt;
}

bool isLinkedVreg(const Operand& operand, int id) {
  if (!operand.isLinked()) {
    return false;
  }
  const Instruction* def = operand.getLinkedInstr();
  return def != nullptr && def->id() == id;
}

bool matchesInputImm(const Instruction& ins, size_t index, uint64_t imm) {
  if (ins.getNumInputs() <= index) {
    return false;
  }
  const Operand* in = ins.getInput(index);
  return in != nullptr && in->isImm() && in->getConstant() == imm;
}

} // namespace

Query::Query(const Function& func) : func_(func) {}

Query& Query::opcode(Opcode op) {
  opcode_ = op;
  return *this;
}
Query& Query::outType(DataType dt) {
  out_type_ = dt;
  return *this;
}
Query& Query::outVreg(int id) {
  out_vreg_ = id;
  return *this;
}
Query& Query::outInd(
    int base_vreg,
    int32_t offset,
    std::optional<int> index_vreg,
    DataType dt) {
  outType(dt);
  outIndBaseVreg(base_vreg);
  outIndOffset(offset);
  if (index_vreg.has_value()) {
    outIndIndexVreg(*index_vreg);
  } else {
    outIndNoIndex();
  }
  return *this;
}
Query& Query::outInd(int base_vreg, int32_t offset, DataType dt) {
  return outInd(base_vreg, offset, std::nullopt, dt);
}
Query& Query::outIndBaseVreg(int id) {
  out_ind_base_vreg_ = id;
  return *this;
}
Query& Query::outIndIndexVreg(int id) {
  out_ind_index_vreg_ = id;
  out_ind_no_index_ = false;
  return *this;
}
Query& Query::outIndOffset(int32_t offset) {
  out_ind_offset_ = offset;
  return *this;
}
Query& Query::outIndNoIndex() {
  out_ind_no_index_ = true;
  return *this;
}

Query::InputMatch& Query::input(size_t index) {
  for (InputMatch& im : inputs_) {
    if (im.index == index) {
      return im;
    }
  }
  inputs_.push_back(InputMatch{.index = index});
  return inputs_.back();
}

Query& Query::inImm(size_t index, uint64_t v) {
  input(index).imm = v;
  return *this;
}
Query& Query::inAddr(size_t index, uint64_t addr) {
  input(index).addr = addr;
  return *this;
}
Query& Query::inVreg(size_t index, int id) {
  input(index).vreg = id;
  return *this;
}
Query& Query::inType(size_t index, DataType dt) {
  input(index).type = dt;
  return *this;
}
Query&
Query::guard(uint64_t deopt_id, uint64_t frame_index, DataType guard_type) {
  opcode_ = Opcode::kGuard;
  input(0).imm = deopt_id;
  input(1).imm = frame_index;
  input(2).type = guard_type;
  return *this;
}
Query& Query::inDefOpcode(size_t index, Opcode op) {
  input(index).def_opcode = op;
  return *this;
}
Query& Query::inDefImm(size_t index, size_t def_input_index, uint64_t v) {
  input(index).def_inputs.push_back(
      DefInputMatch{.index = def_input_index, .imm = v});
  return *this;
}
Query& Query::with(std::function<bool(const Instruction*)> pred) {
  extra_ = std::move(pred);
  return *this;
}

bool Query::matches(const Instruction& ins) const {
  if (opcode_ && ins.opcode() != *opcode_) {
    return false;
  }
  return matchesOutput(ins) && matchesInputs(ins) && (!extra_ || extra_(&ins));
}

bool Query::matchesOutput(const Instruction& ins) const {
  const Operand* out = ins.output();
  if (out_vreg_ && ins.id() != *out_vreg_) {
    return false;
  }
  if (out_type_) {
    if (out == nullptr || out->dataType() != *out_type_) {
      return false;
    }
  }
  if (out_ind_base_vreg_ || out_ind_index_vreg_ || out_ind_offset_ ||
      out_ind_no_index_) {
    if (out == nullptr || !out->isInd()) {
      return false;
    }
    const MemoryIndirect* ind = out->getMemoryIndirect();
    if (ind == nullptr) {
      return false;
    }
    if (out_ind_base_vreg_) {
      const Operand* base = ind->getBaseRegOperand();
      if (base == nullptr || !isLinkedVreg(*base, *out_ind_base_vreg_)) {
        return false;
      }
    }
    if (out_ind_index_vreg_) {
      const Operand* index = ind->getIndexRegOperand();
      if (index == nullptr || !isLinkedVreg(*index, *out_ind_index_vreg_)) {
        return false;
      }
    }
    if (out_ind_offset_ && ind->getOffset() != *out_ind_offset_) {
      return false;
    }
    if (out_ind_no_index_ && ind->getIndexRegOperand() != nullptr) {
      return false;
    }
  }
  return true;
}

bool Query::matchesInputs(const Instruction& ins) const {
  for (const InputMatch& im : inputs_) {
    if (!matchesInput(ins, im)) {
      return false;
    }
  }
  return true;
}

bool Query::matchesInput(const Instruction& ins, const InputMatch& im) const {
  if (ins.getNumInputs() <= im.index) {
    return false;
  }
  const Operand* in = ins.getInput(im.index);
  if (in == nullptr) {
    return false;
  }
  if (im.imm && (!in->isImm() || in->getConstant() != *im.imm)) {
    return false;
  }
  if (im.addr && immOrMemoryAddress(*in) != im.addr) {
    return false;
  }
  if (im.vreg && !isLinkedVreg(*in, *im.vreg)) {
    return false;
  }
  if (im.type && in->dataType() != *im.type) {
    return false;
  }
  if (!matchesInputDef(*in, im)) {
    return false;
  }
  return true;
}

bool Query::matchesInputDef(const Operand& in, const InputMatch& im) const {
  if (!im.def_opcode && im.def_inputs.empty()) {
    return true;
  }
  if (!in.isLinked()) {
    return false;
  }
  const Instruction* def = in.getLinkedInstr();
  if (def == nullptr) {
    return false;
  }
  if (im.def_opcode && def->opcode() != *im.def_opcode) {
    return false;
  }
  for (const DefInputMatch& def_input : im.def_inputs) {
    if (def_input.imm &&
        !matchesInputImm(*def, def_input.index, *def_input.imm)) {
      return false;
    }
  }
  return true;
}

const Instruction* Query::find() const {
  for (const BasicBlock* bb : func_.basicBlocks()) {
    for (const std::unique_ptr<Instruction>& instr : bb->instructions()) {
      if (matches(*instr)) {
        return instr.get();
      }
    }
  }
  return nullptr;
}

bool Query::exists() const {
  return find() != nullptr;
}

const Function& Query::func() const {
  return func_;
}

std::string lirFuncString(const Function& func) {
  return fmt::format("{}", func);
}

bool hasLIRSequence(
    const Function& func,
    std::initializer_list<Query> queries) {
  if (queries.size() == 0) {
    return true;
  }

  for (const BasicBlock* bb : func.basicBlocks()) {
    auto next = queries.begin();
    for (const std::unique_ptr<Instruction>& instr : bb->instructions()) {
      if (next->matches(*instr)) {
        ++next;
        if (next == queries.end()) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace cinderx::jit::lir
