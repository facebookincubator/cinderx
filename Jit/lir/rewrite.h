// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/printer.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace jit::lir {

// At what granularity the rewrite is being performed.
enum RewriteType {
  kFunction,
  kBasicBlock,
  kInstruction,
};

// What happened as a result of a rewrite operation.
enum RewriteResult {
  kUnchanged,
  kChanged,
  kRemoved,
};

using function_rewrite_arg_t = Function*;
using basic_block_rewrite_arg_t = BasicBlock*;
using instruction_rewrite_arg_t = instr_iter_t;

template <typename T>
using function_type_t = std::function<RewriteResult(T)>;
using function_rewrite_t = function_type_t<function_rewrite_arg_t>;
using basic_block_rewrite_t = function_type_t<basic_block_rewrite_arg_t>;
using instruction_rewrite_t = function_type_t<instruction_rewrite_arg_t>;

// This class implements a framework for LIR rewrites.
class Rewrite {
 public:
  Rewrite(Function* func, codegen::Environ* env) : function_(func), env_(env) {}

  Function* function() {
    return function_;
  }

  const Function* function() const {
    return function_;
  }

  codegen::Environ* environment() {
    return env_;
  }

  const codegen::Environ* environment() const {
    return env_;
  }

  template <typename T>
  void registerOneRewriteFunction(RewriteResult (*rewrite)(T), int stage = 0) {
    registerOneRewriteFunction(std::function{rewrite}, stage);
  }

  template <typename T>
  void registerOneRewriteFunction(
      RewriteResult (*rewrite)(T, codegen::Environ*),
      int stage = 0) {
    registerOneRewriteFunction(
        function_type_t<T>(
            std::bind(rewrite, std::placeholders::_1, environment())),
        stage);
  }

  template <typename T>
  void registerOneRewriteFunction(
      const std::function<RewriteResult(T)>& rewrite,
      int stage = 0) {
    if constexpr (std::is_same_v<T, function_rewrite_arg_t>) {
      function_rewrites_[stage].push_back(rewrite);
    } else if constexpr (std::is_same_v<T, basic_block_rewrite_arg_t>) {
      basic_block_rewrites_[stage].push_back(rewrite);
    } else if constexpr (std::is_same_v<T, instruction_rewrite_arg_t>) {
      instruction_rewrites_[stage].push_back(rewrite);
    } else {
      static_assert(!sizeof(T*), "Bad rewrite function type.");
    }
  }

  void run();

 protected:
  // find the most recent instruction affecting flags within the
  // basic block. returns nullptr if not found.
  static Instruction* findRecentFlagAffectingInstr(instr_iter_t instr_iter);

 private:
  template <typename T>
  std::pair<bool, const T*> getStageRewrites(
      const std::unordered_map<int, T>& rewrites,
      int stage) {
    auto iter = rewrites.find(stage);
    if (iter == rewrites.end()) {
      return std::make_pair(false, nullptr);
    }

    return std::make_pair(true, &(iter->second));
  }

  void runOneStage(int stage);

  // Keeps doing one type of rewrites until the fixed point is reached.
  // Returns true if the original function has been changed by the rewrites,
  // indicating that all the rewrites have to be run again.
  // Returns false if nothing has been changed in the original function.
  template <typename T, typename V>
  std::enable_if_t<
      std::is_same_v<T, function_rewrite_t> ||
          std::is_same_v<T, basic_block_rewrite_t> ||
          std::is_same_v<T, instruction_rewrite_t>,
      bool>
  runOneTypeRewrites(const std::vector<T>& rewrites, V&& arg) {
    bool changed = false;
    bool loop_changed = false;

    do {
      loop_changed = false;
      for (auto& rewrite : rewrites) {
        auto r = rewrite(std::forward<V>(arg));
        loop_changed |= (r != kUnchanged);

        if (r == kRemoved) {
          return true;
        }
      }

      changed |= loop_changed;
    } while (loop_changed);

    return changed;
  }

  Function* function_;
  codegen::Environ* env_;

  std::unordered_map<int, std::vector<function_type_t<function_rewrite_arg_t>>>
      function_rewrites_;
  std::unordered_map<
      int,
      std::vector<function_type_t<basic_block_rewrite_arg_t>>>
      basic_block_rewrites_;
  std::unordered_map<
      int,
      std::vector<function_type_t<instruction_rewrite_arg_t>>>
      instruction_rewrites_;
};

} // namespace jit::lir
