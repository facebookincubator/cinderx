// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/lir/instruction.h"

#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace cinderx::jit::hir {
class Instr;
} // namespace cinderx::jit::hir

namespace cinderx::jit::lir {

class Function;

// Basic block class for LIR
class BasicBlock {
 public:
  using InstrList = std::list<std::unique_ptr<Instruction>>;
  using instr_iter_t = InstrList::iterator;

  explicit BasicBlock(Function* func);

  // Get the unique ID representing this block within its function.
  int id() const;

  // Change the block's ID.  This is only meant to be used by the LIR
  // parser.  LIR strongly expects unique instruction IDs.
  void setId(int id);

  // Get the function that has this block as part of its CFG.
  Function* function();
  const Function* function() const;

  void addSuccessor(BasicBlock* bb);

  // Change the outgoing edge at index. Remove its phi values from the old
  // successor; this does not add phi values to the new successor.
  void setSuccessor(size_t index, BasicBlock* bb);

  // Remove the last outgoing edge and its associated predecessor and phi
  // values from the successor. Used for the allocator-only trailing resume
  // edge.
  void popSuccessor();

  const std::vector<BasicBlock*>& successors() const;

  void swapSuccessors();

  BasicBlock* getTrueSuccessor() const;
  BasicBlock* getFalseSuccessor() const;

  const std::vector<BasicBlock*>& predecessors() const;

  size_t numPredecessors() const;
  BasicBlock* predecessor(size_t index) const;

  // Pointer-based lookup requires the predecessor to occur at most once.
  std::optional<size_t> findPredecessorIndex(
      const BasicBlock* predecessor) const;
  size_t predecessorIndex(const BasicBlock* predecessor) const;

  // Replace one incoming edge and its phi labels without changing its values.
  void replacePredecessor(BasicBlock* predecessor, BasicBlock* replacement);

  // Remove one incoming edge and its matching label/value pair from each phi.
  void removePredecessor(BasicBlock* predecessor);

  // Allocate an instruction and its operands and append it to the
  // instruction list. For the details on how to allocate instruction
  // operands, please refer to Instruction::addOperands() function.
  template <typename... T>
  Instruction*
  allocateInstr(Opcode opcode, const hir::Instr* origin, T&&... args) {
    instrs_.emplace_back(std::make_unique<Instruction>(this, opcode, origin));
    auto instr = instrs_.back().get();

    instr->addOperands(std::forward<T>(args)...);
    applyPendingAnnotation(instr);
    return instr;
  }

  // Allocate an instruction and its operands and insert it before the
  // instruction specified by iter. For the details on how to allocate
  // instruction operands, please refer to Instruction::addOperands() function.
  template <typename... T>
  Instruction*
  allocateInstrBefore(instr_iter_t iter, Opcode opcode, T&&... args) {
    const hir::Instr* origin = nullptr;
    if (iter != instrs_.end()) {
      origin = (*iter)->origin();
    } else if (iter != instrs_.begin()) {
      origin = (*std::prev(iter))->origin();
    }

    auto instr = std::make_unique<Instruction>(this, opcode, origin);
    auto res = instr.get();
    instrs_.emplace(iter, std::move(instr));

    res->addOperands(std::forward<T>(args)...);
    return res;
  }

  void appendInstr(std::unique_ptr<Instruction> instr);

  std::unique_ptr<Instruction> removeInstr(instr_iter_t iter);

  InstrList& instructions();
  const InstrList& instructions() const;

  bool isEmpty() const;

  size_t getNumInstrs() const;

  Instruction* getFirstInstr();
  const Instruction* getFirstInstr() const;

  Instruction* getLastInstr();
  const Instruction* getLastInstr() const;

  instr_iter_t getLastInstrIter();

  template <typename Func>
  void foreachPhiInstr(const Func& f) const {
    for (auto& instr : instrs_) {
      auto opcode = instr->opcode();
      if (opcode == Opcode::kPhi) {
        f(instr.get());
      }
    }
  }

  // Insert a basic block between this block and the given block.
  BasicBlock* insertBasicBlockBetween(BasicBlock* block);

  // Split this block before instr.
  // Current basic block contains all instructions up to (but excluding) instr.
  // Return a new block with all instructions (including and) after instr.
  BasicBlock* splitBefore(Instruction* instr);

  codegen::CodeSection section() const;
  void setSection(codegen::CodeSection section);

  // Set a pending annotation that will be applied to the next instruction
  // allocated on this block (via applyPendingAnnotation).
  std::string pending_annotation_;

  // Return an iterator to the given instruction. Behavior is undefined if the
  // given Instruction is not in this block.
  //
  // This function is O(getNumInstrs()) due to implementation details in
  // InstrList.
  instr_iter_t iterator_to(Instruction* instr);

 private:
  void appendSuccessor(BasicBlock* successor);
  void addPredecessor(BasicBlock* predecessor);
  void erasePredecessor(size_t index);
  void applyPendingAnnotation(Instruction* instr);
  int id_;
  Function* func_;

  std::vector<BasicBlock*> successors_;
  std::vector<BasicBlock*> predecessors_;

  // Consider using IntrusiveList as in HIR.
  InstrList instrs_;

  codegen::CodeSection section_{codegen::CodeSection::kHot};
};

using instr_iter_t = BasicBlock::instr_iter_t;

} // namespace cinderx::jit::lir
