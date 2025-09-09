// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/rewrite.h"

#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/operand.h"

#include <set>

namespace jit::lir {

Rewrite::Rewrite(Function* func, codegen::Environ* env)
    : function_(func), env_(env) {}

Function* Rewrite::function() {
  return function_;
}

const Function* Rewrite::function() const {
  return function_;
}

codegen::Environ* Rewrite::environment() {
  return env_;
}

const codegen::Environ* Rewrite::environment() const {
  return env_;
}

void Rewrite::run() {
  // collect all stages
  std::set<int> stages;

  for (auto& rewrite : function_rewrites_) {
    stages.insert(rewrite.first);
  }

  for (auto& rewrite : basic_block_rewrites_) {
    stages.insert(rewrite.first);
  }

  for (auto& rewrite : instruction_rewrites_) {
    stages.insert(rewrite.first);
  }

  for (int stage : stages) {
    runOneStage(stage);
  }
}

void Rewrite::runOneStage(int stage) {
  auto [has_function_rewrites, function_rewrites] =
      getStageRewrites(function_rewrites_, stage);
  auto [has_basic_block_rewrites, basic_block_rewrites] =
      getStageRewrites(basic_block_rewrites_, stage);
  auto [has_instruction_rewrites, instruction_rewrites] =
      getStageRewrites(instruction_rewrites_, stage);

  bool changed = false;
  do {
    changed = false;
    if (has_function_rewrites) {
      changed |= runOneTypeRewrites(*function_rewrites, function_);
    }

    if (has_basic_block_rewrites) {
      for (auto& bb : function_->basicblocks()) {
        changed |= runOneTypeRewrites(*basic_block_rewrites, bb);
      }
    }

    if (has_instruction_rewrites) {
      for (auto& bb : function_->basicblocks()) {
        auto& instrs = bb->instructions();

        auto iter = instrs.begin();
        for (iter = instrs.begin(); iter != instrs.end();) {
          auto cur_iter = iter++;

          changed |= runOneTypeRewrites(*instruction_rewrites, cur_iter);
        }
      }
    }
  } while (changed);
}

} // namespace jit::lir
