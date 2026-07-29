// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Jit/bitvector.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinderx::jit::optimizer {

/*
 * This file implements a framework for data-flow analysis based on bit vector
 * operations.  DataFlowAnalyzer is a template class, where the template
 * argument represents the type of objects that each bit is associated to.  It
 * can be an object of a variable, an expression or even a text string
 * description of the bit.
 *
 * Usage:
 *   1. Register every object with addObject().
 *   2. Create one DataFlowBlock per node with createBlock() and wire them up
 *      with connectTo().
 *   3. Set the gen/kill bits for each block.
 *   4. Call solve() with the desired direction and meet operator.
 *
 * An example can be found in dataflow_test.cpp in the RuntimeTests directory.
 */

// Direction of the analysis flow, going between predecessors <-> successors.
enum class Direction {
  Forward,
  Backward,
};

// How the states of adjacent blocks are combined at a join point.  Union yields
// "along some path" facts (e.g. liveness, reaching definitions); Intersect
// yields "along all paths" facts (e.g. definite assignment).
enum class Meet {
  Union,
  Intersect,
};

struct DataFlowBlock {
  void connectTo(DataFlowBlock& block) {
    succ_.insert(&block);
    block.pred_.insert(this);
  }

  util::BitVector gen_;
  util::BitVector kill_;
  util::BitVector in_;
  util::BitVector out_;
  std::unordered_set<DataFlowBlock*> pred_;
  std::unordered_set<DataFlowBlock*> succ_;
};

template <typename T>
class DataFlowAnalyzer {
 public:
  DataFlowAnalyzer() = default;

  // Register an object with the analysis.  Must be called for every object
  // before any blocks are created, as the object count fixes the bit width.
  void addObject(const T& obj) {
    JIT_THROW_IF(
        !blocks_.empty(),
        "Must add all objects to DataFlowAnalyzer before creating blocks");
    obj_to_index_map_.emplace(obj, num_bits_);
    index_to_obj_map_.emplace_back(obj);
    num_bits_++;
  }

  // Create a block owned by the analyzer, sized to hold all registered objects.
  // The returned reference is stable for the lifetime of the analyzer.
  DataFlowBlock& createBlock() {
    DataFlowBlock& block = blocks_.emplace_back();
    block.gen_.setBitWidth(num_bits_);
    block.kill_.setBitWidth(num_bits_);
    block.in_.setBitWidth(num_bits_);
    block.out_.setBitWidth(num_bits_);
    return block;
  }

  void setBlockGenBit(DataFlowBlock& block, const T& bit) {
    block.gen_.setBit(obj_to_index_map_.at(bit));
  }

  void setBlockKillBit(DataFlowBlock& block, const T& bit) {
    block.kill_.setBit(obj_to_index_map_.at(bit));
  }

  bool getBlockInBit(const DataFlowBlock& block, const T& bit) const {
    return block.in_.getBit(obj_to_index_map_.at(bit));
  }

  bool getBlockOutBit(const DataFlowBlock& block, const T& bit) const {
    return block.out_.getBit(obj_to_index_map_.at(bit));
  }

  template <typename F>
  void forEachBlockIn(const DataFlowBlock& block, F per_obj_func) const {
    block.in_.forEachSetBit(
        [&](size_t bit) { per_obj_func(index_to_obj_map_.at(bit)); });
  }

  template <typename F>
  void forEachBlockOut(const DataFlowBlock& block, F per_obj_func) const {
    block.out_.forEachSetBit(
        [&](size_t bit) { per_obj_func(index_to_obj_map_.at(bit)); });
  }

  // Run the analysis to a fixpoint. Every block's transfer function is
  // out = gen | (in - kill); the meaning of `in`/`out` and which neighbors feed
  // the join are determined by `dir`, and how the neighbors are combined by
  // `meet`.
  void solve(Direction dir, Meet meet = Meet::Union) {
    JIT_THROW_IF(
        num_bits_ != obj_to_index_map_.size(),
        "DataFlowAnalyzer: number of bits ({}) doesn't match number of objects "
        "added ({})",
        num_bits_,
        obj_to_index_map_.size());

    const bool forward = dir == Direction::Forward;
    const bool intersect = meet == Meet::Intersect;

    std::deque<DataFlowBlock*> worklist;
    for (auto& block : blocks_) {
      auto& preds = forward ? block.pred_ : block.succ_;
      auto& in = forward ? block.in_ : block.out_;
      auto& out = forward ? block.out_ : block.in_;

      // Intersection analyses must start from the top element (all bits set) so
      // that not-yet-visited neighbors don't spuriously constrain the result.
      if (intersect) {
        out.fill(true);
      }

      if (preds.empty()) {
        // Boundary block: no incoming facts, so its state is fixed. Compute it
        // once and leave it out of the worklist.
        in.fill(false);
        out = block.gen_ | (in - block.kill_);
      } else {
        worklist.push_back(&block);
      }
    }

    while (!worklist.empty()) {
      DataFlowBlock* block = worklist.front();
      worklist.pop_front();

      auto& preds = forward ? block->pred_ : block->succ_;
      auto& succs = forward ? block->succ_ : block->pred_;
      auto& in = forward ? block->in_ : block->out_;
      auto& out = forward ? block->out_ : block->in_;

      util::BitVector new_in;
      bool first = true;
      for (DataFlowBlock* p : preds) {
        const auto& p_out = forward ? p->out_ : p->in_;
        if (first) {
          new_in = p_out;
          first = false;
        } else if (intersect) {
          new_in &= p_out;
        } else {
          new_in |= p_out;
        }
      }

      bool changed = new_in != in;
      in = std::move(new_in);

      auto new_out = block->gen_ | (in - block->kill_);
      changed |= new_out != out;
      out = std::move(new_out);

      if (changed) {
        worklist.insert(worklist.end(), succs.begin(), succs.end());
      }
    }
  }

 private:
  std::unordered_map<T, size_t> obj_to_index_map_;
  std::vector<T> index_to_obj_map_;
  std::deque<DataFlowBlock> blocks_;
  size_t num_bits_{0};
};

} // namespace cinderx::jit::optimizer
