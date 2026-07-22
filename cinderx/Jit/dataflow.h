// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bitvector.h"

#include <algorithm>
#include <functional>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinderx::jit::optimizer {

/* This file implements a framework for data-flow analysis based on bit vector
 * operations. DataFlowAnalyzer is a template class, where the template argument
 * represents the type of objects that each bit is associate to. It can be an
 * object of a variable, an expression or even a text string description of the
 * bit. The class can be instantiated directly or derived for a certain specific
 * analysis, such as DataFlowAnalyzer<std::string> analyzer; OR class
 * LivenessAnalysis: public DataFlowAnalyzer<PyObject *> {
 *         // doing your own cool stuff...
 *     };
 *
 *     LivenessAnalysis analyzer;
 *
 * An example of how to use this class can be found in dataflow_test.h in
 * RuntimeTests directory. It implements the example that can be found in
 * Section 8.1 of the book Advanced Compiler Design And Implementation.
 */

struct DataFlowBlock {
  void connectTo(DataFlowBlock& block) {
    succ_.insert(&block);
    block.pred_.insert(this);
  }

  jit::util::BitVector gen_;
  jit::util::BitVector kill_;
  jit::util::BitVector in_;
  jit::util::BitVector out_;
  std::unordered_set<DataFlowBlock*> pred_;
  std::unordered_set<DataFlowBlock*> succ_;
};

template <typename T>
class DataFlowAnalyzer {
 public:
  DataFlowAnalyzer()
      : num_bits_(0), entry_block_(nullptr), exit_block_(nullptr) {}

  void addBlock(DataFlowBlock& block) {
    blocks_.insert(&block);
    block.gen_.setBitWidth(num_bits_);
    block.kill_.setBitWidth(num_bits_);
    block.in_.setBitWidth(num_bits_);
    block.out_.setBitWidth(num_bits_);
  }

  void setBlockGenBit(DataFlowBlock& block, const T& bit);
  void setBlockGenBits(DataFlowBlock& block, const std::vector<T>& bits);
  void setBlockKillBit(DataFlowBlock& block, const T& bit);
  void setBlockKillBits(DataFlowBlock& block, const std::vector<T>& bits);
  void setEntryBlock(DataFlowBlock& block) {
    entry_block_ = &block;
  }
  void setExitBlock(DataFlowBlock& block) {
    entry_block_ = &block;
  }

  bool getBlockInBit(const DataFlowBlock& block, const T& bit) const;
  bool getBlockOutBit(const DataFlowBlock& block, const T& bit) const;

  template <typename F>
  void forEachBlockIn(const DataFlowBlock& block, F per_obj_func) const;

  template <typename F>
  void forEachBlockOut(const DataFlowBlock& block, F per_obj_func) const;

  void addObject(const T& obj);
  void addObjects(const std::vector<T>& objs);
  size_t getObjectIndex(const T& obj) const;

  // This function does forward-flow analysis when forward is set to true.
  // It does backward-flow analysis otherwise.
  void runAnalysis(bool forward = true);

 private:
  std::unordered_map<T, size_t> obj_to_index_map_;
  std::vector<T> index_to_obj_map_;
  std::unordered_set<DataFlowBlock*> blocks_;
  size_t num_bits_;
  DataFlowBlock* entry_block_;
  DataFlowBlock* exit_block_;
};

template <typename T>
void DataFlowAnalyzer<T>::addObject(const T& obj) {
  obj_to_index_map_.emplace(obj, num_bits_);
  index_to_obj_map_.emplace_back(obj);
  num_bits_++;

  for (auto& block : blocks_) {
    block->gen_.addBits(1);
    block->kill_.addBits(1);
    block->in_.addBits(1);
    block->out_.addBits(1);
  }
}

template <typename T>
void DataFlowAnalyzer<T>::addObjects(const std::vector<T>& objs) {
  for (auto& obj : objs) {
    obj_to_index_map_.emplace(obj, num_bits_);
    index_to_obj_map_.emplace_back(obj);
    num_bits_++;
  }

  auto added_bits = objs.size();
  for (auto& block : blocks_) {
    block->gen_.addBits(added_bits);
    block->kill_.addBits(added_bits);
    block->in_.addBits(added_bits);
    block->out_.addBits(added_bits);
  }
}

template <typename T>
size_t DataFlowAnalyzer<T>::getObjectIndex(const T& obj) const {
  return obj_to_index_map_.at(obj);
}

template <typename T>
template <typename F>
void DataFlowAnalyzer<T>::forEachBlockIn(
    const DataFlowBlock& block,
    F per_obj_func) const {
  block.in_.forEachSetBit(
      [&](size_t bit) { per_obj_func(index_to_obj_map_.at(bit)); });
}

template <typename T>
template <typename F>
void DataFlowAnalyzer<T>::forEachBlockOut(
    const DataFlowBlock& block,
    F per_obj_func) const {
  block.out_.forEachSetBit(
      [&](size_t bit) { per_obj_func(index_to_obj_map_.at(bit)); });
}

template <typename T>
void DataFlowAnalyzer<T>::setBlockGenBit(DataFlowBlock& block, const T& bit) {
  auto pos = obj_to_index_map_.at(bit);
  block.gen_.setBit(pos);
}

template <typename T>
void DataFlowAnalyzer<T>::setBlockGenBits(
    DataFlowBlock& block,
    const std::vector<T>& bits) {
  for (const auto& bit : bits) {
    setBlockGenBit(block, bit);
  }
}

template <typename T>
void DataFlowAnalyzer<T>::setBlockKillBit(DataFlowBlock& block, const T& bit) {
  auto pos = obj_to_index_map_.at(bit);
  block.kill_.setBit(pos);
}

template <typename T>
void DataFlowAnalyzer<T>::setBlockKillBits(
    DataFlowBlock& block,
    const std::vector<T>& bits) {
  for (const auto& bit : bits) {
    setBlockKillBit(block, bit);
  }
}

template <typename T>
bool DataFlowAnalyzer<T>::getBlockInBit(
    const DataFlowBlock& block,
    const T& bit) const {
  auto index = obj_to_index_map_.at(bit);
  return block.in_.getBit(index);
}

template <typename T>
bool DataFlowAnalyzer<T>::getBlockOutBit(
    const DataFlowBlock& block,
    const T& bit) const {
  auto index = obj_to_index_map_.at(bit);
  return block.out_.getBit(index);
}

template <typename T>
void DataFlowAnalyzer<T>::runAnalysis(bool forward) {
  std::list<DataFlowBlock*> blocks;

  std::copy_if(
      blocks_.begin(),
      blocks_.end(),
      std::back_inserter(blocks),
      std::bind(
          std::not_equal_to<DataFlowBlock*>(),
          std::placeholders::_1,
          forward ? entry_block_ : exit_block_));

  jit::util::BitVector bv(num_bits_);
  while (!blocks.empty()) {
    auto block = blocks.front();
    blocks.pop_front();

    auto& pred = forward ? block->pred_ : block->succ_;
    auto& succ = forward ? block->succ_ : block->pred_;
    auto& in = forward ? block->in_ : block->out_;
    auto& out = forward ? block->out_ : block->in_;

    jit::util::BitVector new_in(num_bits_);
    bool changed = false;

    for (auto& p : pred) {
      new_in |= (forward ? p->out_ : p->in_);
    }

    changed |= (new_in != in);
    in = std::move(new_in);

    auto new_out = block->gen_ | (in - block->kill_);
    changed |= (new_out != out);
    out = std::move(new_out);

    if (changed) {
      std::copy(succ.begin(), succ.end(), std::back_inserter(blocks));
    }
  }
}

} // namespace cinderx::jit::optimizer
