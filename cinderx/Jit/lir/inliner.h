// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/lir/function.h"

namespace jit::lir {

class LIRInliner {
 public:
  // Given a function, try to inline all calls.
  // Return true if one or more calls have been inlined (i.e. the function has
  // been modified). Otherwise, return false.
  static bool inlineCalls(Function* function);

  LIRInliner(Function* caller, Instruction* instr);

  // Public function for inlining call_instr_.
  // Return true if inlining succeeds.
  // Return false if inlining cannot be completed
  // and don't modify call_instr_ and its function.
  // NOTE: Assume that callee and caller don't have relative jumps or stack
  // allocation instructions. These instructions should be very infrequent, but
  // we may want to add a check for this later.
  bool inlineCall();

  // Find corresponding function body.  Return nullptr if function cannot be
  // found.
  lir::Function* findCalleeFunction();

 private:
  // The function containing the call instruction.
  Function* caller_{nullptr};
  // The call instruction that we want to inline.
  lir::Instruction* call_instr_;

  // After copying the callee into the caller,
  // callee_start is the index of the first callee block (i.e. the entry block)
  // and callee_end is the index of the last callee block (i.e. the exit block)
  // in caller->basic_blocks_.
  int callee_start_;
  int callee_end_;
  // List of arguments from call_instr_.
  std::vector<lir::OperandBase*> arguments_;

  // Checks if call instruction and callee are inlineable.
  // Calls checkEntryExitReturn, checkArguments, checkLoadArg.
  // Return true if they are inlineable, otherwise return false.
  // NOTE: We may want to extract some of these checks, so that we can apply
  // them as a general pass across all functions.
  bool isInlineable(const lir::Function* callee);

  // Check that there is exactly 1 entry and 1 exit block.
  // Check that these blocks are found at the ends of basic_blocks_.
  // Check that return statements only appear in
  // the predecesoors of the exit block.
  bool checkEntryExitReturn(const lir::Function* callee);

  // Check that call inputs are immediate or virtual registers.
  // Add the inputs to arguments_.
  bool checkArguments();

  // Check that kLoadArg instructions occur at the beginning.
  // Check that kLoadArg instructions don't exceed the number of arguments.
  bool checkLoadArg(const lir::Function* callee);

  // Given the address of the function, try to find the corresponding LIR text
  // and parse it.
  lir::Function* parseFunction(uint64_t addr);

  // Assume that kLoadArg instructions are only found
  // at the beginning of callee_.
  bool resolveArguments();

  // Assume that instr_it corresponds to a kLoadArg instruction.
  // Assume that arguments are immediate or linked.
  void resolveLoadArg(
      UnorderedMap<lir::OperandBase*, lir::LinkedOperand*>& vreg_map,
      lir::BasicBlock* bb,
      instr_iter_t& instr_it);

  // For instr_it that aren't kLoadArg,
  // fix up linked arguments that refer to outputs of kLoadArg instructions.
  void resolveLinkedArgumentsUses(
      UnorderedMap<lir::OperandBase*, lir::LinkedOperand*>& vreg_map,
      std::list<std::unique_ptr<lir::Instruction>>::iterator& instr_it);

  // Expects callee to have one empty epilogue block.
  // Expects return instructions to only appear as
  // the last statement in the predecessors of the epilogue blocks.
  void resolveReturnValue();

  // Get the caller function's name.  Will return a sentinel value if this
  // function was parsed straight from LIR and never had a name.
  std::string_view callerName();

  FRIEND_TEST(LIRInlinerTest, ResolveArgumentsTest);
  FRIEND_TEST(LIRInlinerTest, ResolveReturnWithPhiTest);
  FRIEND_TEST(LIRInlinerTest, ResolveReturnWithoutPhiTest);
};

} // namespace jit::lir
