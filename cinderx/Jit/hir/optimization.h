// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

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

class BuiltinLoadMethodElimination : public Pass {
 public:
  BuiltinLoadMethodElimination() : Pass("BuiltinLoadMethodElimination") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<BuiltinLoadMethodElimination> Factory() {
    return std::make_unique<BuiltinLoadMethodElimination>();
  }
};

} // namespace jit::hir
