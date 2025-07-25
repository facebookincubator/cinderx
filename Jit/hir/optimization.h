// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

// Inserts incref/decref instructions.
class RefcountInsertion : public Pass {
 public:
  RefcountInsertion() : Pass("RefcountInsertion") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<RefcountInsertion> Factory() {
    return std::make_unique<RefcountInsertion>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(RefcountInsertion);
};

// Perform a mixed bag of strength-reduction optimizations: remove redundant
// null checks, conversions, loads from compile-time constant containers, etc.
//
// If your optimization requires no global analysis or state and operates on
// one instruction at a time by inspecting its inputs (and anything reachable
// from them), it may be a good fit for Simplify.
class Simplify : public Pass {
 public:
  Simplify() : Pass("Simplify") {}

  void Run(Function& func) override;

  static std::unique_ptr<Simplify> Factory() {
    return std::make_unique<Simplify>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(Simplify);
};

class DynamicComparisonElimination : public Pass {
 public:
  DynamicComparisonElimination() : Pass("DynamicComparisonElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<DynamicComparisonElimination> Factory() {
    return std::make_unique<DynamicComparisonElimination>();
  }

 private:
  Instr* ReplaceCompare(Compare* compare, IsTruthy* truthy);
  Instr* ReplaceVectorCall(
      Function& irfunc,
      CondBranch& cond_branch,
      BasicBlock& block,
      VectorCall* vectorcall,
      IsTruthy* truthy);

  DISALLOW_COPY_AND_ASSIGN(DynamicComparisonElimination);
};

// Eliminate instructions whose outputs are not used in a return or by
// other instructions with side-effects
class DeadCodeElimination : public Pass {
 public:
  DeadCodeElimination() : Pass("DeadCodeElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<DeadCodeElimination> Factory() {
    return std::make_unique<DeadCodeElimination>();
  }
};

class InsertUpdatePrevInstr : public Pass {
 public:
  InsertUpdatePrevInstr() : Pass("InsertUpdatePrevInstr") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<InsertUpdatePrevInstr> Factory() {
    return std::make_unique<InsertUpdatePrevInstr>();
  }
};

class GuardTypeRemoval : public Pass {
 public:
  GuardTypeRemoval() : Pass("GuardTypeRemoval") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<GuardTypeRemoval> Factory() {
    return std::make_unique<GuardTypeRemoval>();
  }
};

class CleanCFG : public Pass {
 public:
  CleanCFG() : Pass("CleanCFG") {}

  void Run(Function& irfunc) override;

  // Remove blocks that aren't reachable from the entry, whether or not they're
  // empty. Avoid using this alone; use CleanCFG instead. Returns true if it
  // changed the graph and false otherwise.
  static bool RemoveUnreachableBlocks(CFG* cfg);

  // Remove instructions that aren't reachable from the entry. Avoid using this
  // alone; use CleanCFG instead. Returns true if it changed the graph and false
  // otherwise.
  static bool RemoveUnreachableInstructions(CFG* cfg);

  static std::unique_ptr<CleanCFG> Factory() {
    return std::make_unique<CleanCFG>();
  }
};

class InlineFunctionCalls : public Pass {
 public:
  InlineFunctionCalls() : Pass("InlineFunctionCalls") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<InlineFunctionCalls> Factory() {
    return std::make_unique<InlineFunctionCalls>();
  }
};

class BeginInlinedFunctionElimination : public Pass {
 public:
  BeginInlinedFunctionElimination() : Pass("BeginInlinedFunctionElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<BeginInlinedFunctionElimination> Factory() {
    return std::make_unique<BeginInlinedFunctionElimination>();
  }
};

class BuiltinLoadMethodElimination : public Pass {
 public:
  BuiltinLoadMethodElimination() : Pass("BuiltinLoadMethodElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<BuiltinLoadMethodElimination> Factory() {
    return std::make_unique<BuiltinLoadMethodElimination>();
  }
};

} // namespace jit::hir
