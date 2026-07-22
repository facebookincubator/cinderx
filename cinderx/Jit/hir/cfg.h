// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"

namespace cinderx::jit::hir {

class CFG {
 public:
  CFG() = default;
  ~CFG();

  // Allocate a new basic block and insert it into this CFG
  BasicBlock* allocateBlock();

  // Allocate a block without linking it into the CFG
  BasicBlock* allocateUnlinkedBlock();

  // Insert a block into the CFG. The CFG takes ownership and will free it
  // upon destruction of the CFG.
  void insertBlock(BasicBlock* block);

  // Remove block from the CFG
  void removeBlock(BasicBlock* block);

  // Split a block after instr. Once split, the block will contain all
  // instructions up to and including instr. A newly allocated block is returned
  // that contains all instructions following instr.
  BasicBlock* splitAfter(Instr& target);

  // Split any critical edges by inserting trampoline blocks.
  void splitCriticalEdges();

  // Return the RPO traversal of the basic blocks in the CFG starting from
  // entry_block.
  std::vector<BasicBlock*> getRPOTraversal() const;

  // Return the post order traversal of the basic blocks in the CFG starting
  // from entry_block. Used in backward data-flow analysis like unreachable
  // instructions
  std::vector<BasicBlock*> getPostOrderTraversal() const;

  // Return the BasicBlock in the CFG with the specified id, or nullptr if none
  // exist
  const BasicBlock* getBlockById(int id) const;

  // Return the RPO traversal of the reachable basic blocks in the CFG starting
  // from the given block.
  static std::vector<BasicBlock*> getRPOTraversal(BasicBlock* start);

  // Returns the post order traversal of the reachable basic blocks in the CFG
  // starting from the given block. Used in backward data-flow analysis like
  // unreachable instructions
  static std::vector<BasicBlock*> getPostOrderTraversal(BasicBlock* start);

  // Total number of basic blocks in the CFG.
  size_t numBlocks() const;

  // Total number of instructions in the CFG.
  size_t numInstrs() const;

  // Entry point into the CFG; may be null
  BasicBlock* entry_block{nullptr};

  // List of all blocks in the CFG
  IntrusiveList<BasicBlock, &BasicBlock::cfg_node> blocks;

 private:
  DISALLOW_COPY_AND_ASSIGN(CFG);

  int next_block_id_{0};
};

} // namespace cinderx::jit::hir
