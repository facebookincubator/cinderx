// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/dominance.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/hir/cfg.h"

#include <algorithm>

namespace cinderx::jit::hir {

DominatorTree::DominatorTree(BasicBlock* start) {
  rpo_ = CFG::getRPOTraversal(start);
  for (size_t i = 0; i < rpo_.size(); ++i) {
    rpo_index_[rpo_[i]->id] = static_cast<int>(i);
  }

  // Iterative two-finger fixpoint. A predecessor counts as "processed" once it
  // has an entry in idoms_; RPO order guarantees each block has at least one
  // processed predecessor by the time we reach it.
  idoms_[start->id] = start;
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < rpo_.size(); ++i) {
      BasicBlock* block = rpo_[i];
      BasicBlock* new_idom = nullptr;
      for (const Edge* edge : block->inEdges()) {
        BasicBlock* pred = edge->from();
        if (!idoms_.contains(pred->id)) {
          continue;
        }
        new_idom = new_idom == nullptr ? pred : intersect(new_idom, pred);
      }
      JIT_CHECK(
          new_idom != nullptr, "bb {} has no processed predecessor", block->id);
      auto [it, inserted] = idoms_.try_emplace(block->id, new_idom);
      if (inserted || it->second != new_idom) {
        it->second = new_idom;
        changed = true;
      }
    }
  }
  idoms_[start->id] = nullptr;

  for (size_t i = 1; i < rpo_.size(); ++i) {
    children_[immediateDominator(rpo_[i])->id].push_back(rpo_[i]);
  }
  for (auto& [id, kids] : children_) {
    std::sort(kids.begin(), kids.end(), [](BasicBlock* a, BasicBlock* b) {
      return a->id < b->id;
    });
  }
}

const std::vector<BasicBlock*>& DominatorTree::reversePostorder() const {
  return rpo_;
}

// Whether `block` is reachable from the root.
bool DominatorTree::contains(const BasicBlock* block) const {
  return idoms_.contains(block->id);
}

BasicBlock* DominatorTree::intersect(BasicBlock* b1, BasicBlock* b2) const {
  // Advance the deeper finger (higher RPO index) up its idom chain until both
  // fingers meet at the common dominator.
  while (b1 != b2) {
    while (rpo_index_.at(b1->id) < rpo_index_.at(b2->id)) {
      b2 = idoms_.at(b2->id);
    }
    while (rpo_index_.at(b2->id) < rpo_index_.at(b1->id)) {
      b1 = idoms_.at(b1->id);
    }
  }
  return b1;
}

BasicBlock* DominatorTree::immediateDominator(const BasicBlock* block) const {
  auto it = idoms_.find(block->id);
  return it == idoms_.end() ? nullptr : it->second;
}

const std::vector<BasicBlock*>& DominatorTree::children(
    const BasicBlock* block) const {
  static const std::vector<BasicBlock*> kEmpty;
  auto it = children_.find(block->id);
  return it == children_.end() ? kEmpty : it->second;
}

const std::unordered_set<BasicBlock*>& DominatorTree::dominanceFrontier(
    const BasicBlock* block) const {
  if (!frontiers_computed_) {
    computeDominanceFrontiers();
  }
  static const std::unordered_set<BasicBlock*> kEmpty;
  auto it = frontiers_.find(block->id);
  return it == frontiers_.end() ? kEmpty : it->second;
}

void DominatorTree::computeDominanceFrontiers() const {
  frontiers_computed_ = true;
  for (BasicBlock* block : rpo_) {
    std::unordered_set<BasicBlock*> preds;
    for (const Edge* edge : block->inEdges()) {
      if (contains(edge->from())) {
        preds.insert(edge->from());
      }
    }
    if (preds.size() < 2) {
      continue;
    }
    BasicBlock* stop = immediateDominator(block);
    for (BasicBlock* pred : preds) {
      for (BasicBlock* runner = pred; runner != stop;
           runner = immediateDominator(runner)) {
        frontiers_[runner->id].insert(block);
      }
    }
  }
}

const std::unordered_set<const BasicBlock*>&
DominatorTree::getBlocksDominatedBy(const BasicBlock* block) const {
  if (!dom_sets_computed_) {
    computeDominatorSets();
  }
  static const std::unordered_set<const BasicBlock*> kEmpty;
  auto it = dom_sets_.find(block->id);
  return it == dom_sets_.end() ? kEmpty : it->second;
}

bool DominatorTree::sameDominanceAs(const DominatorTree& other) const {
  if (idoms_.size() != other.idoms_.size()) {
    return false;
  }
  // Compare immediate dominators by pointer identity. Both trees are built over
  // the same CFG, so they reference the same BasicBlock objects; this avoids
  // dereferencing a pointer that a missed invalidation may have left dangling.
  for (const auto& [id, idom] : idoms_) {
    auto it = other.idoms_.find(id);
    if (it == other.idoms_.end() || it->second != idom) {
      return false;
    }
  }
  return true;
}

void DominatorTree::computeDominatorSets() const {
  dom_sets_computed_ = true;
  // Walk the blocks bottom-up so each block's dominated set is complete before
  // it is unioned into its immediate dominator's set.
  for (auto it = rpo_.rbegin(); it != rpo_.rend(); ++it) {
    BasicBlock* block = *it;
    auto& block_set = dom_sets_[block->id];
    block_set.insert(block);
    if (BasicBlock* idom = immediateDominator(block)) {
      dom_sets_[idom->id].insert(block_set.begin(), block_set.end());
    }
  }
}

} // namespace cinderx::jit::hir
