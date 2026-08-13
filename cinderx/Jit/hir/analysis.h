// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bitvector.h"
#include "cinderx/Jit/dataflow.h"
#include "cinderx/Jit/hir/dominance.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"

#include <iosfwd>
#include <unordered_map>
#include <unordered_set>

namespace cinderx::jit::hir {

class BasicBlock;
class Register;

using RegisterSet = std::unordered_set<Register*>;
extern const RegisterSet kEmptyRegSet;

std::ostream& operator<<(std::ostream& os, const RegisterSet& set);

// Returns true if each instruction in func properly type-checks
// Writes to err if any failure occurs and returns false
bool funcTypeChecks(const Function& func, std::ostream& err);

// Returns true iff the constraint signifies that all of its instruction's
// operands must match
bool operandsMustMatch(OperandType op_type);

// Returns true if the type satisfies the passed in OperandType
bool registerTypeMatches(Type op_type, OperandType expected_type);

// Collect the registers that some instruction in `func` consumes as data, which
// is every use except a deopt frame-state reference or a UseType assertion.
//
// A register missing from the result is only kept around so a deopt can restore
// it; nothing on the fast path reads it.  That is what lets a pass rewrite
// frame state to hold a cheaper equivalent of the value, such as replacing a
// boxed float with its unboxed source.
RegisterSet collectDataUses(const Function& func);

// Base class for dataflow analyses that compute facts about registers in the
// HIR.
//
// A subclass supplies gen/kill sets per block via computeGenKill() and picks a
// direction and meet operator; the shared solver in DataFlowAnalyzer does the
// rest.
class RegisterAnalysis {
 public:
  virtual ~RegisterAnalysis() = default;

  // Build the dataflow graph from the CFG and solve to a fixpoint. Must be
  // called before querying any results.
  void run();

  RegisterSet getIn(const BasicBlock* block) const;
  RegisterSet getOut(const BasicBlock* block) const;

 protected:
  RegisterAnalysis(
      const Function& irfunc,
      jit::optimizer::Direction dir,
      jit::optimizer::Meet meet)
      : irfunc_{irfunc}, dir_{dir}, meet_{meet} {}

  virtual void computeGenKill(
      const BasicBlock* block,
      RegisterSet& gen,
      RegisterSet& kill) = 0;

  virtual std::string name() const = 0;

  bool inBit(const BasicBlock* block, Register* reg) const;
  bool outBit(const BasicBlock* block, Register* reg) const;

  const Function& irfunc_;
  jit::optimizer::DataFlowAnalyzer<Register*> analyzer_;
  std::unordered_map<const BasicBlock*, jit::optimizer::DataFlowBlock*> blocks_;

 private:
  void dump() const;

  jit::optimizer::Direction dir_;
  jit::optimizer::Meet meet_;
};

class LivenessAnalysis : public RegisterAnalysis {
 public:
  explicit LivenessAnalysis(const Function& irfunc)
      : RegisterAnalysis(
            irfunc,
            jit::optimizer::Direction::Backward,
            jit::optimizer::Meet::Union) {}

  bool isLiveIn(const BasicBlock* block, Register* reg) const {
    return inBit(block, reg);
  }
  bool isLiveOut(const BasicBlock* block, Register* reg) const {
    return outBit(block, reg);
  }

  using LastUses =
      std::unordered_map<const Instr*, std::unordered_set<Register*>>;

  // Compute and return a map indicating which values die after which
  // instructions. Must be called after run().
  LastUses getLastUses();

 protected:
  void computeGenKill(
      const BasicBlock* block,
      RegisterSet& gen,
      RegisterSet& kill) final;

  std::string name() const final {
    return "LivenessAnalysis";
  }
};

// This computes which registers have been initialized at a basic block.
//
// A register is definitely assigned if it has been assigned to along all paths
// into a block. A register is maybe assigned if has been assigned along any
// path to the block.
//
// This information can be used to eliminate null checks for variables that are
// definitely assigned.
//
// NB: This doesn't support DEL_FAST yet (and probably never will).
//
// We probably don't need to run this over temporaries. They should always be
// assigned before being used.
//
// Each bit in the bit-vector represents whether or not the corresponding
// register has been assigned. Local summaries for each block are computed as
// follows:
//
//
//   foreach instruction I in B in order:
//      Gen(B) = Gen(B) U OutputRegister(I)
//
//   Kill(B) = {}  -- could extend this to handle DEL_FAST
//
// Dataflow information is propagated using the following equations:
//
// For definite assignment:
//   In(B) = And(Out(P) for P in Preds(B))
//
// For maybe assignment:
//   In(B) = Or(Out(P) for P in Preds(B))
//
// In both cases:
//   Out(B) = Gen(B) U (In(B) - Kill(B))
//
class AssignmentAnalysis : public RegisterAnalysis {
 public:
  AssignmentAnalysis(const Function& irfunc, bool is_definite);

  bool isAssignedIn(const BasicBlock* block, Register* reg) const {
    return inBit(block, reg);
  }
  bool isAssignedOut(const BasicBlock* block, Register* reg) const {
    return outBit(block, reg);
  }

 protected:
  void computeGenKill(
      const BasicBlock* block,
      RegisterSet& gen,
      RegisterSet& kill) final;

  std::string name() const final {
    return fmt::format(
        "{}AssignmentAnalysis", is_definite_ ? "Definite" : "Maybe");
  }

  RegisterSet args_;

  bool is_definite_;
};

// Stores type information about registers that doesn't get stored in the
// Register's type. This currently means keeping track of `HintType`s and `Phi`s
// which can provide type hints
//
// Since type information might change throughout the program, the analysis
// exposes this type information by allowing users to query for the dominating
// type hint instruction. This gives users access to the potential types as
// well as where that type information was created. The querying is done at the
// BasicBlock level under the assumption that BasicBlocks should be small enough
// that type information that is learned later on in the block should still be
// valid earlier in the block.
class RegisterTypeHints {
 public:
  explicit RegisterTypeHints(const Function& irfunc);

  const Instr* dominatingTypeHint(Register* reg, const BasicBlock* block);

 private:
  // Contains a mapping of Registers to a mapping of BasicBlock ids to type hint
  // instructions.
  // This allows users to query type hints for Registers in a
  // flow-sensitive way
  std::unordered_map<Register*, std::unordered_map<int, const Instr*>>
      dom_hint_;
  DominatorTree doms_;
};

} // namespace cinderx::jit::hir
