// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"

#include <iosfwd>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

// Check that func's CFG is well-formed and that its Register uses and defs are
// vald SSA, returning true iff no errors were found. Details of any errors
// will be written to err.
bool checkFunc(const Function& func, std::ostream& err);

struct SSABasicBlock {
  BasicBlock* block;
  int unsealed_preds;

  std::unordered_set<SSABasicBlock*> preds;
  std::unordered_set<SSABasicBlock*> succs;

  std::unordered_map<Register*, Register*>
      local_defs; // register -> current value
  std::unordered_map<Register*, Phi*>
      phi_nodes; // value -> phi that produced it
  std::vector<std::pair<Register*, Register*>>
      incomplete_phis; // register -> phi output

  explicit SSABasicBlock(BasicBlock* b = nullptr)
      : block(b), unsealed_preds(0) {}
};

class SSAify : public Pass {
 public:
  SSAify() : Pass("SSAify"), env_(nullptr) {}

  void Run(Function& irfunc) override;
  void Run(Function& irfunc, BasicBlock* block);

  static std::unique_ptr<SSAify> Factory() {
    return std::make_unique<SSAify>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(SSAify);

  Register* getDefine(SSABasicBlock* ssa_block, Register* reg);

  // check if the defs going to phi function is trivial
  // return a replacement register if it is trivial
  // return nullptr otherwise.
  Register* getCommonPredValue(
      const Register* out_reg,
      const std::unordered_map<BasicBlock*, Register*>& defs);

  void fixIncompletePhis(SSABasicBlock* ssa_block);

  std::unordered_map<BasicBlock*, SSABasicBlock*> initSSABasicBlocks(
      std::vector<BasicBlock*>& blocks);

  void maybeAddPhi(SSABasicBlock* ssa_block, Register* reg, Register* out);
  Environment* env_;
  std::unordered_map<Register*, std::unordered_map<Phi*, SSABasicBlock*>>
      phi_uses_;
  Register* null_reg_{nullptr};
};

} // namespace jit::hir
