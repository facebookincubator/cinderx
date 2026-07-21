// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cinderx::jit::hir {

// Dominator tree over the blocks reachable from a root block, based on Cooper,
// Harvey, and Kennedy's "A Simple, Fast Dominance Algorithm".
//
// The tree is built from a start block.  The start may be any block, not just
// the CFG entry, so this can be built over an inlined function's sub-CFG.
// Immediate dominators and dominator-tree children are computed up front;
// dominance frontiers and full dominator sets are computed lazily on first use,
// so consumers that only need immediate dominators don't pay for them.
class DominatorTree {
 public:
  // Build the tree rooted at a start block, covering all blocks reachable from
  // it.
  explicit DominatorTree(BasicBlock* start);

  // Reverse-postorder of the reachable blocks, with the root first.
  const std::vector<BasicBlock*>& reversePostorder() const;

  // Whether `block` is reachable from the root.
  bool contains(const BasicBlock* block) const;

  // Immediate dominator of `block`, or nullptr for the root and any block not
  // reachable from it.
  BasicBlock* immediateDominator(const BasicBlock* block) const;

  // Blocks whose immediate dominator is `block`, in ascending-id order.
  const std::vector<BasicBlock*>& children(const BasicBlock* block) const;

  // Dominance frontier of `block`: the blocks where `block`'s dominance stops
  // being exclusive. Computed lazily.
  const std::unordered_set<BasicBlock*>& dominanceFrontier(
      const BasicBlock* block) const;

  // Set of blocks dominated by `block`, inclusive. Computed lazily.
  const std::unordered_set<const BasicBlock*>& getBlocksDominatedBy(
      const BasicBlock* block) const;

  // Whether this tree has the same immediate-dominator mapping as `other`: the
  // same set of reachable blocks, each with the same immediate dominator. Used
  // to detect a stale cached tree against a freshly-computed one.
  bool sameDominanceAs(const DominatorTree& other) const;

 private:
  // Walk up the dominator tree to the common dominator of two blocks.
  BasicBlock* intersect(BasicBlock* b1, BasicBlock* b2) const;
  void computeDominanceFrontiers() const;
  void computeDominatorSets() const;

  std::vector<BasicBlock*> rpo_;
  std::unordered_map<int, int> rpo_index_;
  // Immediate dominators, keyed by block id; the root maps to nullptr.
  std::unordered_map<int, BasicBlock*> idoms_;
  std::unordered_map<int, std::vector<BasicBlock*>> children_;

  mutable std::unordered_map<int, std::unordered_set<BasicBlock*>> frontiers_;
  mutable bool frontiers_computed_{false};
  mutable std::unordered_map<int, std::unordered_set<const BasicBlock*>>
      dom_sets_;
  mutable bool dom_sets_computed_{false};
};

} // namespace cinderx::jit::hir
