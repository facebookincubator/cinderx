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
    fmt::print(os, "{}{}", sep, *reg);
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

RegisterSet collectDataUses(const Function& func) {
  RegisterSet uses;
  for (auto& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      // UseType exists to tell GuardTypeRemoval that a type is relied upon; it
      // is a no-op assertion rather than a consumer of the value.
      if (instr.isUseType()) {
        continue;
      }
      for (std::size_t i = 0, n = instr.numOperands(); i < n; ++i) {
        if (Register* operand = instr.getOperand(i)) {
          uses.insert(operand);
        }
      }
      if (const DeoptBase* deopt = instr.asDeoptBase()) {
        // The guilty register is reported to the deopt machinery, so it has to
        // survive as a real object.  Its frame state deliberately does not
        // count, and neither do its live registers, which are the same kind of
        // restore-only reference (and are empty until RefcountInsertion).
        if (Register* guilty = deopt->guiltyReg()) {
          uses.insert(guilty);
        }
      }
    }
  }
  return uses;
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

void RegisterAnalysis::run() {
  // Register every HIR register as a dataflow object (one bit each). This must
  // happen before any blocks are created, as it fixes the bit width.
  for (const auto& it : irfunc_.env.getRegisters()) {
    analyzer_.addObject(it.second.get());
  }

  // Create a dataflow block per CFG block and record its gen/kill sets.
  for (const auto& cfg_block : irfunc_.cfg.blocks) {
    auto& df_block = analyzer_.createBlock();
    blocks_.emplace(&cfg_block, &df_block);

    RegisterSet gen, kill;
    computeGenKill(&cfg_block, gen, kill);
    for (auto reg : gen) {
      analyzer_.setBlockGenBit(df_block, reg);
    }
    for (auto reg : kill) {
      analyzer_.setBlockKillBit(df_block, reg);
    }
  }

  // Mirror the CFG edges onto the dataflow blocks. The entry block has no
  // predecessors and terminal blocks have no successors; the solver treats
  // those as boundary blocks.
  for (const auto& cfg_block : irfunc_.cfg.blocks) {
    auto* df_block = blocks_.at(&cfg_block);
    for (auto cfg_edge : cfg_block.outEdges()) {
      df_block->connectTo(*blocks_.at(cfg_edge->to()));
    }
  }

  analyzer_.solve(dir_, meet_);

  if (getConfig().log.debug_dataflow_analysis) {
    dump();
  }
}

RegisterSet RegisterAnalysis::getIn(const BasicBlock* block) const {
  RegisterSet in;
  analyzer_.forEachBlockIn(
      *blocks_.at(block), [&](Register* r) { in.insert(r); });
  return in;
}

RegisterSet RegisterAnalysis::getOut(const BasicBlock* block) const {
  RegisterSet out;
  analyzer_.forEachBlockOut(
      *blocks_.at(block), [&](Register* r) { out.insert(r); });
  return out;
}

bool RegisterAnalysis::inBit(const BasicBlock* block, Register* reg) const {
  return analyzer_.getBlockInBit(*blocks_.at(block), reg);
}

bool RegisterAnalysis::outBit(const BasicBlock* block, Register* reg) const {
  return analyzer_.getBlockOutBit(*blocks_.at(block), reg);
}

void RegisterAnalysis::dump() const {
  std::string out = fmt::format("{} complete:\n", name());
  for (auto& block : irfunc_.cfg.blocks) {
    format_to(out, "  bb {}\n", block.id);
    auto format_set = [&](const RegisterSet& regs) {
      for (auto reg : regs) {
        format_to(out, "    {}\n", *reg);
      }
    };
    format_to(out, "  In:\n");
    format_set(getIn(&block));
    format_to(out, "  Out:\n");
    format_set(getOut(&block));
    format_to(out, "\n");
  }

  JIT_LOG("{}", out);
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

  for (auto& pair : blocks_) {
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

AssignmentAnalysis::AssignmentAnalysis(const Function& irfunc, bool is_definite)
    : RegisterAnalysis(
          irfunc,
          jit::optimizer::Direction::Forward,
          is_definite ? jit::optimizer::Meet::Intersect
                      : jit::optimizer::Meet::Union),
      is_definite_{is_definite} {
  for (const auto& instr : *irfunc_.cfg.entry_block) {
    if (instr.isLoadArg()) {
      args_.insert(instr.output());
    }
  }
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
