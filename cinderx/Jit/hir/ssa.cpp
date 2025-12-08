// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/ssa.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/type.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <ostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

namespace {

struct CheckEnv {
  CheckEnv(const Function& func, std::ostream& err)
      : func{func}, err{err}, assign{func, true} {
    assign.Run();
  }

  const Function& func;
  std::ostream& err;
  bool ok{true};

  // Definite assignment analysis. Used to ensure all uses of a register are
  // dominated by its definition.
  AssignmentAnalysis assign;

  // Flow-insensitive map from register definitions to the source
  // block. Tracked separately from `assign` to ensure no register is defined
  // twice, even if the first definition doesn't dominate the second.
  std::unordered_map<const Register*, const BasicBlock*> defs;

  // Current set of defined registers.
  RegisterSet defined;

  // Current block and instruction.
  const BasicBlock* block{nullptr};
  const Instr* instr{nullptr};
};

// Verify the following:
// - All blocks reachable from the entry block are part of this CFG.
// - The CFG's block list contains no unreachable blocks.
// - No reachable blocks have any unreachable predecessors.
// - No blocks have > 1 edge from the same predecessor
bool checkCFG(const Function& func, std::ostream& err) {
  std::queue<const BasicBlock*> queue;
  std::unordered_set<const BasicBlock*> reachable;
  queue.push(func.cfg.entry_block);
  reachable.insert(func.cfg.entry_block);
  while (!queue.empty()) {
    auto block = queue.front();
    queue.pop();

    if (!block->cfg_node.isLinked()) {
      fmt::print(err, "ERROR: Reachable bb {} isn't part of CFG\n", block->id);
      return false;
    }

    for (auto edge : block->out_edges()) {
      auto succ = edge->to();
      if (reachable.emplace(succ).second) {
        queue.push(succ);
      }
    }
  }

  for (auto& block : func.cfg.blocks) {
    if (!reachable.contains(&block)) {
      fmt::print(err, "ERROR: CFG contains unreachable bb {}\n", block.id);
      return false;
    }

    std::unordered_set<BasicBlock*> seen;
    for (auto edge : block.in_edges()) {
      auto pred = edge->from();
      if (!reachable.contains(pred)) {
        fmt::print(
            err,
            "ERROR: bb {} has unreachable predecessor bb {}\n",
            block.id,
            pred->id);
        return false;
      }
      if (seen.contains(pred)) {
        fmt::print(
            err,
            "ERROR: bb {} has > 1 edge from predecessor bb {}\n",
            block.id,
            pred->id);
        return false;
      }
      seen.insert(pred);
    }
  }

  return true;
}

void checkPhi(CheckEnv& env) {
  auto& phi = static_cast<const Phi&>(*env.instr);
  auto block = phi.block();
  std::unordered_set<const BasicBlock*> preds;
  for (auto edge : block->in_edges()) {
    preds.emplace(edge->from());
  }
  for (auto phi_block : phi.basic_blocks()) {
    if (!preds.contains(phi_block)) {
      fmt::print(
          env.err,
          "ERROR: Instruction '{}' in bb {} references bb {}, which isn't a "
          "predecessor\n",
          phi,
          block->id,
          phi_block->id);
      env.ok = false;
    }
  }
}

void checkTerminator(CheckEnv& env) {
  auto is_last = env.instr == &env.block->back();
  if (env.instr->IsTerminator() && !is_last) {
    fmt::print(
        env.err,
        "ERROR: bb {} contains terminator '{}' in non-terminal position\n",
        env.block->id,
        *env.instr);
    env.ok = false;
  }
  if (is_last && !env.instr->IsTerminator()) {
    fmt::print(
        env.err, "ERROR: bb {} has no terminator at end\n", env.block->id);
    env.ok = false;
  }
}

void checkRegisters(CheckEnv& env) {
  if (env.instr->IsPhi()) {
    auto phi = static_cast<const Phi*>(env.instr);
    for (size_t i = 0; i < phi->NumOperands(); ++i) {
      auto operand = phi->GetOperand(i);
      if (!env.assign.IsAssignedOut(phi->basic_blocks()[i], operand)) {
        fmt::print(
            env.err,
            "ERROR: Phi input '{}' to instruction '{}' in bb {} not "
            "defined at end of bb {}\n",
            operand->name(),
            *phi,
            env.block->id,
            phi->basic_blocks()[i]->id);
        env.ok = false;
      }
    }
  } else {
    for (size_t i = 0, n = env.instr->NumOperands(); i < n; ++i) {
      auto operand = env.instr->GetOperand(i);
      if (!env.defined.contains(operand)) {
        fmt::print(
            env.err,
            "ERROR: Operand '{}' of instruction '{}' not defined at use in "
            "bb {}\n",
            operand->name(),
            *env.instr,
            env.block->id);
        env.ok = false;
      }
    }
  }

  if (auto output = env.instr->output()) {
    if (output->instr() != env.instr) {
      fmt::print(
          env.err,
          "ERROR: {}'s instr is not '{}', which claims to define it\n",
          output->name(),
          *env.instr);
      env.ok = false;
    }

    auto pair = env.defs.emplace(output, env.block);
    if (!pair.second) {
      fmt::print(
          env.err,
          "ERROR: {} redefined in bb {}; previous definition was in bb {}\n",
          output->name(),
          env.block->id,
          pair.first->second->id);
      env.ok = false;
    }
    env.defined.insert(output);
  }
}
} // namespace

// Verify the following properties:
//
// - The CFG is well-formed (see checkCFG() for details).
// - Every block has exactly one terminator instruction, as its final
//   instruction. This implies that blocks cannot be empty, which is also
//   verified.
// - Phi instructions do not appear after any non-Phi instructions in their
//   block.
// - Phi instructions only reference direct predecessors.
// - No register is assigned to by more than one instruction.
// - Every register has a link to its defining instruction.
// - All uses of a register are dominated by its definition.
bool checkFunc(const Function& func, std::ostream& err) {
  if (!checkCFG(func, err)) {
    return false;
  }

  CheckEnv env{func, err};
  for (auto& block : func.cfg.blocks) {
    env.block = &block;
    env.defined = env.assign.GetIn(&block);

    if (block.empty()) {
      fmt::print(err, "ERROR: bb {} has no instructions\n", block.id);
      env.ok = false;
      continue;
    }

    bool phi_section = true;
    bool allow_prologue_loads = env.block == func.cfg.entry_block;
    for (auto& instr : block) {
      env.instr = &instr;

      if (instr.IsPhi()) {
        if (!phi_section) {
          fmt::print(
              err,
              "ERROR: '{}' in bb {} comes after non-Phi instruction\n",
              instr,
              block.id);
          env.ok = false;
          continue;
        }
        checkPhi(env);
      } else {
        phi_section = false;
      }

      if (instr.IsLoadArg() || instr.IsLoadCurrentFunc()) {
        if (!allow_prologue_loads) {
          fmt::print(
              err,
              "ERROR: '{}' in bb {} comes after non-LoadArg instruction\n",
              instr,
              block.id);
          env.ok = false;
        }
      } else {
        allow_prologue_loads = false;
      }

      checkTerminator(env);
      checkRegisters(env);
    }
  }

  return env.ok;
}

void SSAify::Run(Function& irfunc) {
  Run(irfunc, irfunc.cfg.entry_block);
  PhiElimination{}.Run(irfunc);
}

// This implements the algorithm outlined in "Simple and Efficient Construction
// of Static Single Assignment Form"
// https://pp.info.uni-karlsruhe.de/uploads/publikationen/braun13cc.pdf
void SSAify::Run(Function& irfunc, BasicBlock* start) {
  env_ = &irfunc.env;

  auto blocks = CFG::GetRPOTraversal(start);
  auto ssa_basic_blocks = initSSABasicBlocks(blocks);
  phi_uses_.clear();

  for (auto& block : blocks) {
    auto ssablock = ssa_basic_blocks.at(block);

    for (auto& instr : *block) {
      JIT_CHECK(!instr.IsPhi(), "SSAify does not support Phis in its input");
      instr.visitUses([&](Register*& reg) {
        JIT_CHECK(
            reg != nullptr, "Instructions should not have nullptr operands.");
        reg = getDefine(ssablock, reg);
        return true;
      });

      auto out_reg = instr.output();

      if (out_reg != nullptr) {
        auto new_reg = env_->AllocateRegister();
        instr.setOutput(new_reg);
        ssablock->local_defs[out_reg] = new_reg;
      }
    }

    for (auto& succ : ssablock->succs) {
      succ->unsealed_preds--;
      if (succ->unsealed_preds > 0) {
        continue;
      }
      fixIncompletePhis(succ);
    }
  }

  // realize phi functions
  for (auto& bb : ssa_basic_blocks) {
    auto block = bb.first;
    auto ssablock = bb.second;

    // Collect and sort to stabilize IR ordering.
    std::vector<Phi*> phis;
    for (auto& pair : ssablock->phi_nodes) {
      phis.push_back(pair.second);
    }
    std::sort(phis.begin(), phis.end(), [](const Phi* a, const Phi* b) -> bool {
      // Sort using > instead of the typical < because we're effectively
      // reversing by looping push_front below.
      return a->output()->id() > b->output()->id();
    });
    for (auto& phi : phis) {
      block->push_front(phi);
    }

    delete ssablock;
  }

  reflowTypes(irfunc, start);
}

Register* SSAify::getDefine(SSABasicBlock* ssablock, Register* reg) {
  auto iter = ssablock->local_defs.find(reg);
  if (iter != ssablock->local_defs.end()) {
    // If defined locally, just return
    return iter->second;
  }

  if (ssablock->preds.size() == 0) {
    // If we made it back to the entry block and didn't find a definition, use
    // a Nullptr from LoadConst. Place it after the initialization of the args
    // which explicitly come first.
    if (null_reg_ == nullptr) {
      auto it = ssablock->block->begin();
      while (it != ssablock->block->end() &&
             (it->IsLoadArg() || it->IsLoadCurrentFunc())) {
        ++it;
      }
      null_reg_ = env_->AllocateRegister();
      auto loadnull = LoadConst::create(null_reg_, TNullptr);
      loadnull->copyBytecodeOffset(*it);
      loadnull->InsertBefore(*it);
    }
    ssablock->local_defs.emplace(reg, null_reg_);
    return null_reg_;
  }

  if (ssablock->unsealed_preds > 0) {
    // If we haven't visited all our predecessors, they can't provide
    // definitions for us to look up. We'll place an incomplete phi that will
    // be resolved once we've visited all predecessors.
    auto phi_output = env_->AllocateRegister();
    ssablock->incomplete_phis.emplace_back(reg, phi_output);
    ssablock->local_defs.emplace(reg, phi_output);
    return phi_output;
  }

  if (ssablock->preds.size() == 1) {
    // If we only have a single predecessor, use its value
    auto new_reg = getDefine(*ssablock->preds.begin(), reg);
    ssablock->local_defs.emplace(reg, new_reg);
    return new_reg;
  }

  // We have multiple predecessors and may need to create a phi.
  auto new_reg = env_->AllocateRegister();
  // Adding a phi may loop back to our block if there is a loop in the CFG.  We
  // update our local_defs before adding the phi to terminate the recursion
  // rather than looping infinitely.
  ssablock->local_defs.emplace(reg, new_reg);
  maybeAddPhi(ssablock, reg, new_reg);

  return ssablock->local_defs.at(reg);
}

void SSAify::maybeAddPhi(
    SSABasicBlock* ssa_block,
    Register* reg,
    Register* out) {
  std::unordered_map<BasicBlock*, Register*> pred_defs;
  for (auto& pred : ssa_block->preds) {
    auto pred_reg = getDefine(pred, reg);
    pred_defs.emplace(pred->block, pred_reg);
  }
  auto bc_off = ssa_block->block->begin()->bytecodeOffset();
  auto phi = Phi::create(out, pred_defs);
  phi->setBytecodeOffset(bc_off);
  ssa_block->phi_nodes.emplace(out, phi);
  for (auto& def_pair : pred_defs) {
    phi_uses_[def_pair.second].emplace(phi, ssa_block);
  }
}

Register* SSAify::getCommonPredValue(
    const Register* out_reg,
    const std::unordered_map<BasicBlock*, Register*>& defs) {
  Register* other_reg = nullptr;

  for (auto& def_pair : defs) {
    auto def = def_pair.second;

    if (def == out_reg) {
      continue;
    }

    if (other_reg != nullptr && def != other_reg) {
      return nullptr;
    }

    other_reg = def;
  }

  return other_reg;
}

void SSAify::fixIncompletePhis(SSABasicBlock* ssa_block) {
  for (auto& pi : ssa_block->incomplete_phis) {
    maybeAddPhi(ssa_block, pi.first, pi.second);
  }
}

std::unordered_map<BasicBlock*, SSABasicBlock*> SSAify::initSSABasicBlocks(
    std::vector<BasicBlock*>& blocks) {
  std::unordered_map<BasicBlock*, SSABasicBlock*> ssa_basic_blocks;

  auto get_or_create_ssa_block =
      [&ssa_basic_blocks](BasicBlock* block) -> SSABasicBlock* {
    auto iter = ssa_basic_blocks.find(block);
    if (iter == ssa_basic_blocks.end()) {
      auto ssablock = new SSABasicBlock(block);
      ssa_basic_blocks.emplace(block, ssablock);
      return ssablock;
    }
    return iter->second;
  };

  for (auto& block : blocks) {
    auto ssablock = get_or_create_ssa_block(block);
    for (auto& edge : block->out_edges()) {
      auto succ = edge->to();
      auto succ_ssa_block = get_or_create_ssa_block(succ);
      auto p = succ_ssa_block->preds.insert(ssablock);
      if (p.second) {
        // It's possible that we have multiple outgoing edges to the same
        // successor. Since we only care about the number of unsealed
        // predecessor *nodes*, only update if this is the first time we're
        // processing this predecessor.
        succ_ssa_block->unsealed_preds++;
        ssablock->succs.insert(succ_ssa_block);
      }
    }
  }

  return ssa_basic_blocks;
}

} // namespace jit::hir
