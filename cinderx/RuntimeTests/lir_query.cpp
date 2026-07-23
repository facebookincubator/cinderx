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

} // namespace

Query::Query(const Function& func) : func_(func) {}

Query& Query::opcode(Instruction::Opcode op) {
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
  if (out_vreg_ && ins.id() != *out_vreg_) {
    return false;
  }
  if (out_type_) {
    const Operand* out = ins.output();
    if (out == nullptr || out->dataType() != *out_type_) {
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
