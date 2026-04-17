// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/lir/block.h"

#include <deque>
#include <string>
#include <vector>

namespace jit::hir {
class Function;
}

namespace jit::lir {

class Function {
 public:
  struct CopyResult {
    int begin_bb;
    int end_bb;
  };

  explicit Function(const hir::Function* hir_func = nullptr);

  // Allocate a new ID for a basic block or an instruction.
  int allocateId();

  // Set the next ID to return from allocateId().  Only meant to be used by the
  // LIR parser.
  void setNextId(int id);

  // Deep copy function into dest_func.
  // Insert the blocks between prev_bb and next_bb.
  // Assumes that prev_bb and next_bb appear consecutively
  // in dest_func->basic_blocks_.
  // Returns the range of inserted blocks in dest_func->basic_blocks_.
  // The inserted blocks start at (inclusive) dest_func->basic_blocks_[begin_bb]
  // and end right before (exclusive) dest_func->basic_blocks_[begin_bb].
  CopyResult copyFrom(
      const Function* src_func,
      BasicBlock* prev_bb,
      BasicBlock* next_bb,
      const hir::Instr* origin);

  // Create a new block and insert it as the last block in the CFG.
  BasicBlock* allocateBasicBlock();

  // Create a new block and insert it in a given spot in the CFG.
  BasicBlock* allocateBasicBlockAfter(BasicBlock* block);

  // Returns the list of all the basic blocks.
  // The basic blocks will be in RPO as long as the CFG has not been
  // modified since the last call to SortRPO().
  const std::vector<BasicBlock*>& basicblocks() const;
  std::vector<BasicBlock*>& basicblocks();

  BasicBlock* entryBlock() const;

  size_t getNumBasicBlocks() const;

  void sortBasicBlocks();

  // Set/get the exit block — the final block containing the epilogue.
  // For non-generators this is the single exit block; for generators it is
  // exit_epilogue_ (the shared epilogue after the return/yield merge).
  // The block sorter uses this to ensure the exit block is always placed
  // last, regardless of allocation order.
  void setExitBlock(BasicBlock* block) {
    exit_block_ = block;
  }
  BasicBlock* exitBlock() const {
    return exit_block_;
  }

  // Set/get the generator resume entry block. During block sorting, this
  // block's successors (resume blocks) are kept in the sorted list for
  // register allocation, but the resume entry block itself is excluded —
  // it is a placeholder populated post-regalloc by PopulateResumeEntryBlock.
  // It is re-inserted into the block list in generateCode() before emission.
  void setResumeEntryBlock(BasicBlock* block) {
    resume_entry_block_ = block;
  }
  BasicBlock* resumeEntryBlock() const {
    return resume_entry_block_;
  }

  const hir::Function* hirFunc() const;

  // Associate a debug annotation string with an instruction. The annotation
  // covers that instruction and all subsequent instructions until the next
  // annotated instruction or end of block (used by PYTHONJITDUMPASM=1).
  void annotate(const Instruction* instr, std::string text) {
    annotations_.emplace(instr, std::move(text));
  }

  const std::string* getAnnotation(const Instruction* instr) const {
    auto it = annotations_.find(instr);
    return it != annotations_.end() ? &it->second : nullptr;
  }

 private:
  const hir::Function* hir_func_;

  // The containers below hold all the basic blocks for the Function. The deque
  // holds the actual data for blocks and the vector holds their (eventually)
  // sorted order.
  //
  // We use a deque for the data as it provides relatively cheap append
  // (compared to a list) while also keeping value locations in memory constant.
  // Note the basic_block_store_ may end up holding some dead blocks after
  // sorting. However this doesn't matter so much as the overall Function
  // object shouldn't hang around for too long.
  //
  // The other obvious way to implement this would be to have just basic_blocks_
  // as std::vector<unique_ptr<BasicBlock>>, or std::list<BasicBlock>. However,
  // both of these proved to have surprisingly bad performance in practice.
  // This approach gave a roughly 33% perf improvement over the vector of
  // unique_ptrs for a pathalogically large function.
  std::deque<BasicBlock> basic_block_store_;
  // NOTE: The first basic block should always be the entry basic block,
  // where the function starts.
  std::vector<BasicBlock*> basic_blocks_;

  // The exit block containing the epilogue. Set explicitly by the LIR
  // generator so the block sorter doesn't have to rely on positional
  // assumptions (e.g. back() being the exit block).
  BasicBlock* exit_block_{nullptr};

  // Generator resume entry block — its successors (resume blocks) are kept
  // in the block list by sortBasicBlocks, but this block itself is excluded
  // during regalloc and re-inserted in generateCode() after being populated.
  BasicBlock* resume_entry_block_{nullptr};

  // The next id to assign to a BasicBlock or Instruction.
  int next_id_{0};

  // Debug annotation map: instruction → label string for PYTHONJITDUMPASM.
  UnorderedMap<const Instruction*, std::string> annotations_;
};

} // namespace jit::lir
