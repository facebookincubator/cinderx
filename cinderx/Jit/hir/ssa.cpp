// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/ssa.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/dominance.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/type.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <ostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cinderx::jit::hir {

namespace {

struct CheckEnv {
  CheckEnv(const Function& func, std::ostream& err)
      : func{func}, err{err}, assign{func, true} {
    assign.run();
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

    for (auto edge : block->outEdges()) {
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
    for (auto edge : block.inEdges()) {
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
  for (auto edge : block->inEdges()) {
    preds.emplace(edge->from());
  }
  for (auto phi_block : phi.basicBlocks()) {
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
  if (env.instr->isTerminator() && !is_last) {
    fmt::print(
        env.err,
        "ERROR: bb {} contains terminator '{}' in non-terminal position\n",
        env.block->id,
        *env.instr);
    env.ok = false;
  }
  if (is_last && !env.instr->isTerminator()) {
    fmt::print(
        env.err, "ERROR: bb {} has no terminator at end\n", env.block->id);
    env.ok = false;
  }
}

void checkRegisters(CheckEnv& env) {
  if (env.instr->isPhi()) {
    auto phi = static_cast<const Phi*>(env.instr);
    for (size_t i = 0; i < phi->numOperands(); ++i) {
      auto operand = phi->getOperand(i);
      if (!env.assign.isAssignedOut(phi->basicBlocks()[i], operand)) {
        fmt::print(
            env.err,
            "ERROR: Phi input '{}' to instruction '{}' in bb {} not "
            "defined at end of bb {}\n",
            operand->name(),
            *phi,
            env.block->id,
            phi->basicBlocks()[i]->id);
        env.ok = false;
      }
    }
  } else {
    for (size_t i = 0, n = env.instr->numOperands(); i < n; ++i) {
      auto operand = env.instr->getOperand(i);
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
    env.defined = env.assign.getIn(&block);

    if (block.empty()) {
      fmt::print(err, "ERROR: bb {} has no instructions\n", block.id);
      env.ok = false;
      continue;
    }

    bool phi_section = true;
    bool allow_prologue_loads = env.block == func.cfg.entry_block;
    for (auto& instr : block) {
      env.instr = &instr;

      if (instr.isPhi()) {
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

      if (instr.isLoadArg() || instr.isLoadCurrentFunc() ||
          instr.isLoadFrame()) {
        if (!allow_prologue_loads) {
          fmt::print(
              err,
              "ERROR: '{}' in bb {} comes after non-Load* instruction\n",
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

namespace {

// Static single assignment construction using the classic algorithm of Cytron,
// Ferrante, Rosen, Wegman, and Zadeck, "Efficiently Computing Static Single
// Assignment Form and the Control Dependence Graph" (TOPLAS 1991). Phi nodes
// are placed at the iterated dominance frontiers of each variable's definition
// sites, then a walk of the dominator tree renames uses to their reaching
// definitions.
//
// The input is HIR where a single "variable" Register may be defined by more
// than one instruction; the output gives each definition a fresh SSA Register
// and inserts Phis where control flow merges distinct definitions. The pass
// operates on the sub-CFG reachable from `start`, which lets it run over an
// inlined function's body in isolation.
class SSAConstructor {
 public:
  SSAConstructor(Environment& env, BasicBlock* start)
      : env_{env}, start_{start}, dom_{start} {}

  void run();

 private:
  struct PhiInfo {
    // The SSA Register produced by the Phi; allocated when its block is
    // renamed.
    Register* output{nullptr};
    // Incoming value from each predecessor block, filled during renaming.
    std::unordered_map<BasicBlock*, Register*> args;
  };

  void computeLiveness();
  void collectDefSites();
  void placePhis();
  void rename();
  void renameBlock(BasicBlock* block, std::vector<Register*>& pushed);
  void materializePhis();

  // Current reaching definition of `var`, or a null constant if it is undefined
  // on this path.
  Register* readVar(Register* var);
  Register* getNullReg();
  std::vector<BasicBlock*> sortedSuccessors(BasicBlock* block);

  Environment& env_;
  BasicBlock* start_;
  DominatorTree dom_;

  // Variables live on entry to each block, used to place pruned SSA (a Phi is
  // only created where its variable is actually needed).
  std::unordered_map<BasicBlock*, std::unordered_set<Register*>> live_in_;

  std::unordered_map<Register*, std::unordered_set<BasicBlock*>> def_sites_;
  std::unordered_map<BasicBlock*, std::unordered_map<Register*, PhiInfo>> phis_;
  // Stack of reaching definitions per variable, scoped to the dominator-tree
  // walk.
  std::unordered_map<Register*, std::vector<Register*>> def_stack_;

  Register* null_reg_{nullptr};
};

void SSAConstructor::run() {
  computeLiveness();
  collectDefSites();
  placePhis();
  rename();
  materializePhis();
}

// Backward liveness of the pre-SSA variables over the reachable subgraph.
void SSAConstructor::computeLiveness() {
  const std::vector<BasicBlock*>& rpo = dom_.reversePostorder();
  std::unordered_map<BasicBlock*, std::unordered_set<Register*>> gen;
  std::unordered_map<BasicBlock*, std::unordered_set<Register*>> kill;
  for (BasicBlock* block : rpo) {
    auto& block_gen = gen[block];
    auto& block_kill = kill[block];
    for (Instr& instr : *block) {
      instr.visitUses([&](Register* reg) {
        if (!block_kill.contains(reg)) {
          block_gen.insert(reg);
        }
        return true;
      });
      if (Register* out = instr.output()) {
        block_kill.insert(out);
      }
    }
  }

  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = rpo.rbegin(); it != rpo.rend(); ++it) {
      BasicBlock* block = *it;
      std::unordered_set<Register*> new_in = gen[block];
      for (BasicBlock* succ : sortedSuccessors(block)) {
        for (Register* reg : live_in_[succ]) {
          if (!kill[block].contains(reg)) {
            new_in.insert(reg);
          }
        }
      }
      if (new_in != live_in_[block]) {
        live_in_[block] = std::move(new_in);
        changed = true;
      }
    }
  }
}

void SSAConstructor::collectDefSites() {
  for (BasicBlock* block : dom_.reversePostorder()) {
    for (Instr& instr : *block) {
      JIT_CHECK(!instr.isPhi(), "SSAify does not support Phis in its input");
      if (Register* out = instr.output()) {
        def_sites_[out].insert(block);
      }
    }
  }
}

// Place Phis at the iterated dominance frontier of each variable's definitions.
void SSAConstructor::placePhis() {
  for (auto& [var, sites] : def_sites_) {
    std::vector<BasicBlock*> worklist{sites.begin(), sites.end()};
    std::unordered_set<BasicBlock*> on_worklist{sites.begin(), sites.end()};
    std::unordered_set<BasicBlock*> has_phi;
    while (!worklist.empty()) {
      BasicBlock* x = worklist.back();
      worklist.pop_back();
      for (BasicBlock* y : dom_.dominanceFrontier(x)) {
        // Pruned SSA: skip Phis for variables that aren't live at the merge.
        if (!live_in_[y].contains(var)) {
          continue;
        }
        if (!has_phi.insert(y).second) {
          continue;
        }
        // A Phi is itself a definition of `var`, so `y` joins the worklist to
        // propagate to its own dominance frontier.
        phis_[y].try_emplace(var);
        if (on_worklist.insert(y).second) {
          worklist.push_back(y);
        }
      }
    }
  }
}

void SSAConstructor::rename() {
  struct Frame {
    BasicBlock* block;
    bool exit;
  };

  // Registers pushed onto def_stack_ while renaming a block, to be popped when
  // the dominator-tree walk leaves it.
  std::unordered_map<BasicBlock*, std::vector<Register*>> pushed;
  std::vector<Frame> stack;
  stack.push_back({start_, false});

  while (!stack.empty()) {
    Frame frame = stack.back();
    stack.pop_back();

    if (frame.exit) {
      for (Register* var : pushed[frame.block]) {
        def_stack_[var].pop_back();
      }
      pushed.erase(frame.block);
      continue;
    }

    renameBlock(frame.block, pushed[frame.block]);
    stack.push_back({frame.block, true});
    // Push children in reverse so they are visited in ascending-id order.
    const std::vector<BasicBlock*>& children = dom_.children(frame.block);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({*it, false});
    }
  }
}

void SSAConstructor::renameBlock(
    BasicBlock* block,
    std::vector<Register*>& pushed) {
  // Phi outputs are the reaching definitions at block entry. Allocate them in
  // variable-id order so register numbering is deterministic.
  if (auto phi_it = phis_.find(block); phi_it != phis_.end()) {
    std::vector<Register*> vars;
    vars.reserve(phi_it->second.size());
    for (auto& [var, info] : phi_it->second) {
      vars.push_back(var);
    }
    std::sort(vars.begin(), vars.end(), [](Register* a, Register* b) {
      return a->id() < b->id();
    });
    for (Register* var : vars) {
      Register* out = env_.allocateRegister();
      phi_it->second[var].output = out;
      def_stack_[var].push_back(out);
      pushed.push_back(var);
    }
  }

  for (Instr& instr : *block) {
    instr.visitUses([&](Register*& reg) {
      JIT_CHECK(
          reg != nullptr, "Instructions should not have nullptr operands");
      reg = readVar(reg);
      return true;
    });
    if (Register* var = instr.output()) {
      Register* new_reg = env_.allocateRegister();
      instr.setOutput(new_reg);
      def_stack_[var].push_back(new_reg);
      pushed.push_back(var);
    }
  }

  // Supply the operand each successor Phi reads from this block.
  for (BasicBlock* succ : sortedSuccessors(block)) {
    auto it = phis_.find(succ);
    if (it == phis_.end()) {
      continue;
    }
    for (auto& [var, info] : it->second) {
      info.args[block] = readVar(var);
    }
  }
}

void SSAConstructor::materializePhis() {
  for (auto& [block, phi_map] : phis_) {
    auto bc_off = block->begin()->bytecodeOffset();

    // Collect and sort to stabilize IR ordering.
    std::vector<Phi*> phis;
    phis.reserve(phi_map.size());
    for (auto& [var, info] : phi_map) {
      JIT_DCHECK(info.output != nullptr, "Phi output was never allocated");
      Phi* phi = Phi::create(info.output, info.args);
      phi->setBytecodeOffset(bc_off);
      phis.push_back(phi);
    }
    std::sort(phis.begin(), phis.end(), [](const Phi* a, const Phi* b) {
      // Sort using > instead of the typical < because we're effectively
      // reversing by looping push_front below.
      return a->output()->id() > b->output()->id();
    });
    for (Phi* phi : phis) {
      block->push_front(phi);
    }
  }
}

Register* SSAConstructor::readVar(Register* var) {
  auto it = def_stack_.find(var);
  if (it != def_stack_.end() && !it->second.empty()) {
    return it->second.back();
  }
  // The variable is undefined on this path; model it with a null constant
  // placed after the argument-loading prologue in the entry block.
  return getNullReg();
}

Register* SSAConstructor::getNullReg() {
  if (null_reg_ == nullptr) {
    auto it = start_->begin();
    while (it != start_->end() &&
           (it->isLoadArg() || it->isLoadCurrentFunc() || it->isLoadFrame())) {
      ++it;
    }
    null_reg_ = env_.allocateRegister();
    auto loadnull = LoadConst::create(null_reg_, TNullptr);
    loadnull->copyBytecodeOffset(*it);
    loadnull->insertBefore(*it);
  }
  return null_reg_;
}

std::vector<BasicBlock*> SSAConstructor::sortedSuccessors(BasicBlock* block) {
  std::unordered_set<BasicBlock*> seen;
  std::vector<BasicBlock*> succs;
  for (const Edge* edge : block->outEdges()) {
    BasicBlock* to = edge->to();
    if (dom_.contains(to) && seen.insert(to).second) {
      succs.push_back(to);
    }
  }
  std::sort(succs.begin(), succs.end(), [](BasicBlock* a, BasicBlock* b) {
    return a->id < b->id;
  });
  return succs;
}

} // namespace

void SSAify::run(Function& irfunc) {
  run(irfunc, irfunc.cfg.entry_block);
  PhiElimination{}.run(irfunc);
}

void SSAify::run(Function& irfunc, BasicBlock* start) {
  SSAConstructor{irfunc.env, start}.run();
  reflowTypes(irfunc, start);
}

} // namespace cinderx::jit::hir
