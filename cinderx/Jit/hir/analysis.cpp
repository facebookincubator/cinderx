// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/analysis.h"

#include "cinderx/Jit/dataflow.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <memory>

namespace cinderx::jit::hir {

const RegisterSet kEmptyRegSet;

std::ostream& operator<<(std::ostream& os, const RegisterSet& regs) {
  fmt::print(os, "RegisterSet[{}] = {{", regs.size());
  std::vector<Register*> sorted_regs{regs.begin(), regs.end()};
  std::sort(sorted_regs.begin(), sorted_regs.end(), [](auto r1, auto r2) {
    return r1->id() < r2->id();
  });
  auto sep = "";
  for (auto reg : sorted_regs) {
    fmt::print(os, "{}{}", sep, reg->name());
    sep = ", ";
  }
  return os << "}";
}

static bool isSingleCInt(Type t) {
  return t <= TCInt8 || t <= TCUInt8 || t <= TCInt16 || t <= TCUInt16 ||
      t <= TCInt32 || t <= TCUInt32 || t <= TCInt64 || t <= TCUInt64;
}

bool registerTypeMatches(Type op_type, OperandType expected_type) {
  switch (expected_type.kind) {
    case Constraint::kType:
      return op_type <= expected_type.type;
    case Constraint::kTupleExactOrCPtr:
      return op_type <= TTupleExact || op_type <= TCPtr;
    case Constraint::kListOrChkList:
      return op_type <= TList ||
          (op_type.hasTypeSpec() &&
           Ci_CheckedList_TypeCheck(op_type.typeSpec()));
    case Constraint::kDictOrChkDict:
      return op_type <= TDict ||
          (op_type.hasTypeSpec() &&
           Ci_CheckedDict_TypeCheck(op_type.typeSpec()));
    case Constraint::kOptObjectOrCIntOrCBool:
      return op_type <= TOptObject || op_type <= TCInt || op_type <= TCBool;
    case Constraint::kOptObjectOrCInt:
      return op_type <= TOptObject || op_type <= TCInt;
    case Constraint::kMatchAllAsCInt:
      return isSingleCInt(op_type);
    case Constraint::kMatchAllAsCIntOrCBool:
      return isSingleCInt(op_type) || op_type <= TCBool;
    case Constraint::kMatchAllAsPrimitive:
      return isSingleCInt(op_type) || op_type <= TCBool ||
          op_type <= TCDouble || op_type <= TCPtr;
  }
  JIT_ABORT("unknown constraint");
}

bool operandsMustMatch(OperandType op_type) {
  switch (op_type.kind) {
    case Constraint::kMatchAllAsCInt:
    case Constraint::kMatchAllAsCIntOrCBool:
    case Constraint::kMatchAllAsPrimitive:
      return true;

    case Constraint::kType:
    case Constraint::kTupleExactOrCPtr:
    case Constraint::kListOrChkList:
    case Constraint::kDictOrChkDict:
    case Constraint::kOptObjectOrCInt:
    case Constraint::kOptObjectOrCIntOrCBool:
      return false;
  }
  JIT_ABORT("unknown constraint");
}

bool funcTypeChecks(const Function& func, std::ostream& err) {
  for (auto& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      if (instr.numOperands() > 1 &&
          operandsMustMatch(instr.getOperandType(0))) {
        Type join = TBottom;
        for (std::size_t i = 0; i < instr.numOperands(); i++) {
          JIT_DCHECK(
              operandsMustMatch(instr.getOperandType(i)),
              "Inconsistent operand type constraint");
          join |= instr.getOperand(i)->type();
        }
        OperandType expected_type = instr.getOperandType(0);
        if (!registerTypeMatches(join, expected_type)) {
          fmt::print(
              err,
              "TYPE MISMATCH in bb {} of '{}'\nInstr '{}' expected "
              "join of operands of type {} to subclass '{}'\n",
              block.id,
              func.fullname,
              instr,
              join,
              expected_type);
          return false;
        }
      } else {
        for (std::size_t i = 0; i < instr.numOperands(); i++) {
          Register* op = instr.getOperand(i);
          OperandType expected_type = instr.getOperandType(i);
          if (!registerTypeMatches(op->type(), expected_type)) {
            fmt::print(
                err,
                "TYPE MISMATCH in bb {} of '{}'\nInstr '{}' expected "
                "operand {} to be of type {} "
                "but got {} from '{}'\n",
                block.id,
                func.fullname,
                instr,
                i,
                expected_type,
                op->type(),
                *op->instr());
            return false;
          }
        }
      }
    }
  }
  return true;
}

void DataflowAnalysis::addBasicBlock(const BasicBlock* cfg_block) {
  auto res = df_blocks_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(cfg_block),
      std::forward_as_tuple());
  auto& df_block = res.first->second;
  df_analyzer_.addBlock(df_block);
  setUninitialized(&df_block);

  std::unordered_set<Register*> gen, kill;
  computeGenKill(cfg_block, gen, kill);

  for (auto reg : gen) {
    df_analyzer_.setBlockGenBit(df_block, reg);
  }
  for (auto reg : kill) {
    df_analyzer_.setBlockKillBit(df_block, reg);
  }
}

void DataflowAnalysis::initialize() {
  // Add all registers -- this sets up the correct number of bits for the
  // analysis
  num_bits_ = irfunc_.env.getRegisters().size();
  for (const auto& it : irfunc_.env.getRegisters()) {
    df_analyzer_.addObject(it.second.get());
  }

  // Compute the initial state for each block
  for (const auto& cfg_block : irfunc_.cfg.blocks) {
    addBasicBlock(&cfg_block);
  }

  // Set up dataflow graph
  df_analyzer_.addBlock(df_entry_);
  df_analyzer_.setEntryBlock(df_entry_);

  df_analyzer_.addBlock(df_exit_);
  df_analyzer_.setExitBlock(df_exit_);

  for (const auto& cfg_block : irfunc_.cfg.blocks) {
    auto& df_block = df_blocks_[&cfg_block];

    if (&cfg_block == irfunc_.cfg.entry_block) {
      df_entry_.connectTo(df_block);
    }

    if (cfg_block.outEdges().empty()) {
      df_block.connectTo(df_exit_);
    } else {
      for (auto cfg_edge : cfg_block.outEdges()) {
        auto succ_cfg_block = cfg_edge->to();
        JIT_CHECK(
            df_blocks_.contains(succ_cfg_block),
            "succ_cfg_block has to be in the hash table df_blocks_.");
        auto& succ_df_block = df_blocks_.at(succ_cfg_block);
        df_block.connectTo(succ_df_block);
      }
    }
  }
}

RegisterSet DataflowAnalysis::getIn(const BasicBlock* cfg_block) {
  RegisterSet in;
  const auto& df_block = df_blocks_[cfg_block];
  df_analyzer_.forEachBlockIn(df_block, [&](Register* r) { in.insert(r); });
  return in;
}

RegisterSet DataflowAnalysis::getOut(const BasicBlock* cfg_block) {
  RegisterSet out;
  const auto& df_block = df_blocks_[cfg_block];
  df_analyzer_.forEachBlockOut(df_block, [&](Register* r) { out.insert(r); });
  return out;
}

void DataflowAnalysis::dump() {
  if (!getConfig().log.debug) {
    return;
  }

  std::string out = fmt::format("{} complete:\n", name());
  for (auto& block : irfunc_.cfg.blocks) {
    format_to(out, "  bb {}\n", block.id);
    auto format_set = [&](const RegisterSet& regs) {
      for (auto reg : regs) {
        format_to(out, "    {}\n", reg->name());
      }
    };
    format_to(out, "  In:\n");
    format_set(getIn(&block));
    format_to(out, "  Out:\n");
    format_set(getOut(&block));
    format_to(out, "\n");
  }

  JIT_DLOG("{}", out);
}

void BackwardDataflowAnalysis::run() {
  initialize();

  std::list<jit::optimizer::DataFlowBlock*> blocks;
  for (auto& it : df_blocks_) {
    if (&it.second != &df_entry_) {
      blocks.emplace_back(&it.second);
    }
  }

  while (!blocks.empty()) {
    auto block = blocks.front();
    blocks.pop_front();

    auto new_out = computeNewOut(block);
    bool changed = (new_out != block->out_);
    block->out_ = std::move(new_out);

    auto new_in = computeNewIn(block);
    changed |= (new_in != block->in_);
    block->in_ = std::move(new_in);

    if (changed) {
      std::copy(
          block->pred_.begin(), block->pred_.end(), std::back_inserter(blocks));
    }
  }
}

void ForwardDataflowAnalysis::run() {
  initialize();

  std::list<jit::optimizer::DataFlowBlock*> blocks;
  for (auto& it : df_blocks_) {
    if (&it.second != &df_exit_) {
      blocks.emplace_back(&it.second);
    }
  }

  while (!blocks.empty()) {
    auto block = blocks.front();
    blocks.pop_front();

    auto new_in = computeNewIn(block);
    bool changed = (new_in != block->in_);
    block->in_ = std::move(new_in);

    auto new_out = computeNewOut(block);
    changed |= (new_out != block->out_);
    block->out_ = std::move(new_out);

    if (changed) {
      blocks.insert(blocks.end(), block->succ_.begin(), block->succ_.end());
    }
  }
}

template <typename OutputFunc, typename UseFunc>
static void analyzeInstrLiveness(
    const Instr& instr,
    OutputFunc define_output,
    UseFunc use) {
  if (auto output = instr.output()) {
    define_output(output);
  }

  if (instr.isPhi()) {
    // Phi uses happen at the end of the predecessor block.
    return;
  }

  instr.visitUses([&](Register* reg) {
    use(reg);
    return true;
  });

  if (instr.numEdges() > 0) {
    // Mark any Phi inputs on successors to this block as live. When we switch
    // to Branch passing arguments to blocks rather than using Phis, this will
    // happen naturally as the Branch is processed.
    for (size_t i = 0, n = instr.numEdges(); i < n; ++i) {
      auto succ = instr.successor(i);
      int phi_idx = -1;
      for (auto& succ_instr : *succ) {
        if (!succ_instr.isPhi()) {
          break;
        }
        auto& phi = static_cast<const Phi&>(succ_instr);
        if (phi_idx == -1) {
          phi_idx = phi.blockIndex(instr.block());
        }
        use(phi.getOperand(phi_idx));
      }
    }
  }
}

LivenessAnalysis::LastUses LivenessAnalysis::getLastUses() {
  LastUses last_uses;

  for (auto& pair : df_blocks_) {
    auto block = pair.first;
    auto live = getOut(block);

    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      auto& instr = *it;
      analyzeInstrLiveness(
          instr,
          [&](Register* output) {
            if (live.erase(output) == 0) {
              // output isn't live after instr. It's dead and dies right after
              // definition.
              last_uses[&instr].emplace(output);
            }
          },
          [&](Register* value) {
            if (live.emplace(value).second) {
              // value isn't live after instr, so this is a last use.
              last_uses[&instr].emplace(value);
            }
          });
    }
  }

  return last_uses;
}

void LivenessAnalysis::computeGenKill(
    const BasicBlock* cfg_block,
    RegisterSet& gen,
    RegisterSet& kill) {
  for (auto it = cfg_block->rbegin(); it != cfg_block->rend(); ++it) {
    analyzeInstrLiveness(
        *it,
        [&](Register* output) {
          kill.insert(output);
          gen.erase(output);
        },
        [&](Register* use) { gen.insert(use); });
  }
}

jit::util::BitVector LivenessAnalysis::computeNewIn(
    const jit::optimizer::DataFlowBlock* block) {
  jit::util::BitVector new_in(num_bits_);
  new_in = block->gen_ | (block->out_ - block->kill_);
  return new_in;
}

jit::util::BitVector LivenessAnalysis::computeNewOut(
    const jit::optimizer::DataFlowBlock* block) {
  jit::util::BitVector new_out(num_bits_);
  for (auto& succ : block->succ_) {
    new_out |= succ->in_;
  }
  return new_out;
}

void LivenessAnalysis::setUninitialized(jit::optimizer::DataFlowBlock*) {
  // Do nothing.
}

bool LivenessAnalysis::isLiveIn(const BasicBlock* cfg_block, Register* reg) {
  const auto& df_block = df_blocks_[cfg_block];
  return df_analyzer_.getBlockInBit(df_block, reg);
}

bool LivenessAnalysis::isLiveOut(const BasicBlock* cfg_block, Register* reg) {
  const auto& df_block = df_blocks_[cfg_block];
  return df_analyzer_.getBlockOutBit(df_block, reg);
}

AssignmentAnalysis::AssignmentAnalysis(const Function& irfunc, bool is_definite)
    : ForwardDataflowAnalysis(irfunc), args_(), is_definite_(is_definite) {
  for (const auto& instr : *irfunc_.cfg.entry_block) {
    if (instr.isLoadArg()) {
      args_.insert(instr.output());
    }
  }
}

bool AssignmentAnalysis::isAssignedIn(
    const BasicBlock* cfg_block,
    Register* reg) {
  const auto& df_block = df_blocks_[cfg_block];
  return df_analyzer_.getBlockInBit(df_block, reg);
}

bool AssignmentAnalysis::isAssignedOut(
    const BasicBlock* cfg_block,
    Register* reg) {
  const auto& df_block = df_blocks_[cfg_block];
  return df_analyzer_.getBlockOutBit(df_block, reg);
}

void AssignmentAnalysis::computeGenKill(
    const BasicBlock* block,
    RegisterSet& gen,
    RegisterSet& /* kill */) {
  gen = args_;
  for (const auto& instr : *block) {
    auto output = instr.output();
    if (output != nullptr) {
      gen.insert(output);
    }
  }
}

jit::util::BitVector AssignmentAnalysis::computeNewIn(
    const jit::optimizer::DataFlowBlock* block) {
  if (block->pred_.empty()) {
    jit::util::BitVector new_in(num_bits_);
    return new_in;
  }
  auto it = block->pred_.begin();
  auto pred = *it++;
  jit::util::BitVector new_in = pred->out_;
  while (it != block->pred_.end()) {
    if (is_definite_) {
      new_in &= (*it)->out_;
    } else {
      new_in |= (*it)->out_;
    }
    it++;
  }
  return new_in;
}

jit::util::BitVector AssignmentAnalysis::computeNewOut(
    const jit::optimizer::DataFlowBlock* block) {
  jit::util::BitVector new_out(num_bits_);
  new_out = block->gen_ | (block->in_ - block->kill_);
  return new_out;
}

void AssignmentAnalysis::setUninitialized(
    jit::optimizer::DataFlowBlock* block) {
  if (is_definite_) {
    block->out_.fill(true);
  }
}

RegisterTypeHints::RegisterTypeHints(const Function& irfunc)
    : doms_{irfunc.cfg.entry_block} {
  for (const auto& block : irfunc.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.isHintType()) {
        for (size_t i = 0; i < instr.numOperands(); i++) {
          dom_hint_[instr.getOperand(i)][block.id] = &instr;
        }
      } else if (instr.isPhi()) {
        dom_hint_[instr.output()][block.id] = &instr;
      }
    }
  }
}

const Instr* RegisterTypeHints::dominatingTypeHint(
    Register* reg,
    const BasicBlock* block) {
  // Make sure we don't default construct the map for untracked registers
  auto it = dom_hint_.find(reg);
  if (it == dom_hint_.end()) {
    return nullptr;
  }
  std::unordered_map<int, const Instr*> hint_types = it->second;
  // Look for the first type hint that dominates the passed in block
  while (!hint_types[block->id]) {
    block = doms_.immediateDominator(block);
    if (block == nullptr) {
      return nullptr;
    }
  }
  return hint_types[block->id];
}

} // namespace cinderx::jit::hir
