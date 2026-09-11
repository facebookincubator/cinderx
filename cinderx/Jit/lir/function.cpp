// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/function.h"

#include "cinderx/Common/containers.h"
#include "cinderx/Jit/lir/blocksorter.h"

#include <algorithm>

namespace cinderx::jit::lir {

namespace {

using CopiedEdgeMap =
    UnorderedMap<const BasicBlock*, UnorderedMap<size_t, IncomingEdge>>;

// Helper for copyOperand.
void copyIndirect(
    UnorderedMap<Operand*, int>& instr_refs,
    Operand* dest_op,
    MemoryIndirect* source_op) {
  auto base = source_op->getBaseRegOperand();
  auto index = source_op->getIndexRegOperand();
  std::variant<Instruction*, PhyLocation> dest_base;
  std::variant<Instruction*, PhyLocation> dest_index;
  if (base->isLinked()) {
    dest_base = dest_op->instr();
  } else {
    // Otherwise, it must be physical register.
    dest_base = base->getPhyRegister();
  }
  if (index != nullptr) {
    if (index->isLinked()) {
      dest_index = dest_op->instr();
    } else {
      // Otherwise, it must be physical register.
      dest_index = index->getPhyRegister();
    }
  }

  dest_op->setMemoryIndirect(
      dest_base,
      dest_index,
      source_op->getMultiplier(),
      source_op->getOffset());

  // add linked operands to instr_refs
  auto memInd = dest_op->getMemoryIndirect();
  if (base->isLinked()) {
    auto base_linked_id = base->getLinkedOperand()->instr()->id();
    instr_refs.emplace(memInd->getBaseRegOperand(), base_linked_id);
  }

  if (index != nullptr && index->isLinked()) {
    auto index_linked_id = index->getLinkedOperand()->instr()->id();
    instr_refs.emplace(memInd->getIndexRegOperand(), index_linked_id);
  }
}

// Helper for copyOperand.
// Assume that type and data type are already be set.
void copyOperand(
    UnorderedMap<int, BasicBlock*>& block_index_map,
    UnorderedMap<Operand*, int>& instr_refs,
    Operand* operand,
    Operand* operand_copy) {
  switch (operand->type()) {
    case Operand::kReg: {
      operand_copy->setPhyRegister(operand->getPhyRegister());
      operand_copy->setDataType(operand->dataType());
      break;
    }
    case Operand::kStack: {
      operand_copy->setStackSlot(operand->getStackSlot());
      operand_copy->setDataType(operand->dataType());
      break;
    }
    case Operand::kMem: {
      operand_copy->setMemoryAddress(operand->getMemoryAddress());
      break;
    }
    case Operand::kImm: {
      operand_copy->setConstant(operand->getConstant(), operand->dataType());
      break;
    }
    case Operand::kLabel: {
      operand_copy->setBasicBlock(
          map_get_strict(block_index_map, operand->getBasicBlock()->id()));
      break;
    }
    case Operand::kInd: {
      copyIndirect(instr_refs, operand_copy, operand->getMemoryIndirect());
      break;
    }
    case Operand::kNone:
    case Operand::kVreg:
      // operand_copy should already be type kVreg.
      break;
  }
}

// Helper for deepCopyBasicBlocks.
std::unique_ptr<Operand> copyInput(
    UnorderedMap<int, BasicBlock*>& block_index_map,
    UnorderedMap<Operand*, int>& instr_refs,
    Operand* input,
    Instruction* instr_copy) {
  auto input_copy = std::make_unique<Operand>(instr_copy);
  if (input->isLinked()) {
    instr_refs.emplace(input_copy.get(), input->getDefine()->instr()->id());
  } else {
    copyOperand(block_index_map, instr_refs, input, input_copy.get());
    input_copy->setDataType(input->dataType());
  }
  return input_copy;
}

// Helper for deepCopyBasicBlocks.
void connectLinkedOperands(
    UnorderedMap<int, Instruction*>& output_index_map_,
    UnorderedMap<Operand*, int>& instr_refs_) {
  for (auto& [operand, instr_index] : instr_refs_) {
    auto instr = map_get_strict(output_index_map_, instr_index);
    operand->setLinkedInstr(instr);
  }
}

// Helper used in copyFrom.
// Expects blocks to be initialized into block_index_map_.
// Copies the instructions and successors from src_blocks.
void deepCopyBasicBlocks(
    const std::vector<BasicBlock*>& src_blocks,
    UnorderedMap<int, BasicBlock*>& block_index_map_,
    const hir::Instr* origin) {
  UnorderedMap<int, Instruction*> output_index_map;
  UnorderedMap<Operand*, int> instr_refs;
  CopiedEdgeMap copied_edges;

  for (auto bb : src_blocks) {
    BasicBlock* bb_copy = map_get_strict(block_index_map_, bb->id());
    for (size_t outgoing_slot = 0; outgoing_slot < bb->successors().size();
         ++outgoing_slot) {
      BasicBlock* succ = bb->successors()[outgoing_slot];
      IncomingEdge edge =
          bb_copy->addSuccessor(map_get_strict(block_index_map_, succ->id()));
      copied_edges[succ].emplace(
          bb->outgoingEdge(outgoing_slot).incomingSlot(), edge);
    }
  }

  for (auto bb : src_blocks) {
    BasicBlock* bb_copy = map_get_strict(block_index_map_, bb->id());
    for (auto& instr : bb->instructions()) {
      // Copying the instruction will also copy the output
      // (including the output type and data type).
      auto instruction = instr->isPhi()
          ? Instruction::makePhi(bb_copy, instr.get(), origin)
          : std::make_unique<Instruction>(bb_copy, instr.get(), origin);
      bb_copy->instructions().emplace_back(std::move(instruction));
      Instruction* instr_copy = bb_copy->instructions().back().get();
      output_index_map.emplace(instr->id(), instr_copy);
      // Copy output.
      Operand* output = instr->output();
      Operand* output_copy = instr_copy->output();
      copyOperand(block_index_map_, instr_refs, output, output_copy);
      // Copy inputs.
      if (instr->isPhi()) {
        for (size_t i = 0; i < instr->numPhiInputs(); ++i) {
          auto& incoming_edges = map_get_strict(copied_edges, bb);
          instr_copy->addPhiInput(
              map_get_strict(incoming_edges, i),
              copyInput(
                  block_index_map_,
                  instr_refs,
                  instr->phiInput(i),
                  instr_copy));
        }
      } else {
        for (size_t i = 0, n = instr->getNumInputs(); i < n; ++i) {
          Operand* input = instr->getInput(i);
          instr_copy->appendInput(
              copyInput(block_index_map_, instr_refs, input, instr_copy));
        }
      }
    }
  }

  connectLinkedOperands(output_index_map, instr_refs);
}

} // namespace

Function::Function(const hir::Function* hir_func) : hir_func_{hir_func} {}

int Function::allocateId() {
  return next_id_++;
}

void Function::setNextId(int id) {
  next_id_ = id;
}

Function::CopyResult Function::copyFrom(
    const Function* src_func,
    BasicBlock* prev_bb,
    BasicBlock* next_bb,
    const hir::Instr* origin) {
  JIT_CHECK(
      prev_bb->successors().size() == 1 && prev_bb->successors()[0] == next_bb,
      "prev_bb should only have 1 successor which should be next_bb.");
  const size_t next_bb_incoming_slot = prev_bb->outgoingEdge(0).incomingSlot();

  UnorderedMap<int, BasicBlock*> block_index_map;

  // Initialize the basic blocks.
  for (auto bb : src_func->basicBlocks()) {
    BasicBlock* bb_copy = &basic_block_store_.emplace_back(this);
    block_index_map.emplace(bb->id(), bb_copy);
    // Insert basic block before the last block.
    basic_blocks_.emplace(std::prev(basic_blocks_.end()), bb_copy);
  }

  deepCopyBasicBlocks(src_func->basicBlocks(), block_index_map, origin);

  int end = basic_blocks_.size() - 1;
  int start = end - src_func->basic_blocks_.size();
  BasicBlock* dest_start = basic_blocks_.at(start);
  BasicBlock* dest_end = basic_blocks_.at(end - 1);
  prev_bb->setSuccessor(0, next_bb_incoming_slot, dest_start);
  JIT_CHECK(
      dest_end->successors().empty(),
      "Last block of function should have no successors.");
  dest_end->addSuccessor(next_bb);

  return CopyResult{start, end};
}

BasicBlock* Function::allocateBasicBlock() {
  basic_block_store_.emplace_back(this);
  BasicBlock* new_block = &basic_block_store_.back();
  basic_blocks_.emplace_back(new_block);
  return new_block;
}

BasicBlock* Function::allocateBasicBlockAfter(BasicBlock* block) {
  auto iter = std::find_if(
      basic_blocks_.begin(),
      basic_blocks_.end(),
      [block](const BasicBlock* a) -> bool { return block == a; });
  ++iter;
  basic_block_store_.emplace_back(this);
  BasicBlock* new_block = &basic_block_store_.back();
  basic_blocks_.emplace(iter, new_block);
  return new_block;
}

const std::vector<BasicBlock*>& Function::basicBlocks() const {
  return basic_blocks_;
}

std::vector<BasicBlock*>& Function::basicBlocks() {
  return basic_blocks_;
}

BasicBlock* Function::entryBlock() const {
  if (basic_blocks_.empty()) {
    return nullptr;
  }
  return basic_blocks_.front();
}

size_t Function::getNumBasicBlocks() const {
  return basic_blocks_.size();
}

size_t Function::getNumInstrs() const {
  size_t n = 0;
  for (const BasicBlock* block : basic_blocks_) {
    n += block->getNumInstrs();
  }
  return n;
}

void Function::sortBasicBlocks() {
  // Remove resume_entry_block from the block list before sorting.
  // It is a placeholder with no instructions or CFG edges during regalloc and
  // must not participate in pre-regalloc block ordering or liveness analysis.
  // PopulateResumeEntryBlock fills it after allocation.
  //
  // Resume blocks are reachable from yield blocks via allocator-only CFG
  // edges. The resume_entry_block has no CFG edges and is re-inserted into the
  // block list in generateCode() before code emission.
  if (resume_entry_block_ != nullptr) {
    std::erase(basic_blocks_, resume_entry_block_);
  }

  // Use the explicitly tracked exit block. Fall back to back() for
  // compatibility with tests that don't call setExitBlock().
  BasicBlock* exit = exit_block_ ? exit_block_ : basic_blocks_.back();
  BasicBlockSorter sorter(basic_blocks_, exit);
  auto result = sorter.sort();
  basic_blocks_ = std::move(result.sorted_blocks);

  if (result.pruned_blocks.empty()) {
    return;
  }

  for (BasicBlock* block : basic_blocks_) {
    block->removePredecessorsIf([&](BasicBlock* predecessor) {
      return result.pruned_blocks.contains(predecessor);
    });
  }
}

const hir::Function* Function::hirFunc() const {
  return hir_func_;
}

} // namespace cinderx::jit::lir
