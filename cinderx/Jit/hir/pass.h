// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace cinderx::jit::hir {

// An abstract compiler pass over an HIR function.
class Pass {
 public:
  explicit Pass(std::string_view name) : name_{name} {}
  virtual ~Pass() = default;

  virtual void run(Function& irfunc) = 0;

  constexpr std::string_view name() const {
    return name_;
  }

 protected:
  std::string name_;
};

// General utilities that a compiler pass might want to make use of.

using RegUses = std::unordered_map<Register*, std::unordered_set<Instr*>>;

// Recursively chase a list of assignments and get the original register value.
// If there are no assignments then just get the register back.
Register* chaseAssignOperand(Register* value);

// Take a phi instruction and try to collapse it into a new assignment
// instruction if it is trivial (merges in only one other value).  If it's not
// trivial return nullptr.  If it would turn into a malformed assignment
// (`A = Phi A`), then return a load of TBottom instead.
//
// The caller owns the returned instruction and is responsible for linking it
// into a block.
Instr* collapseTrivialPhi(Phi& phi);

// Collect direct operand uses of all Registers in the given func, excluding
// uses in FrameState or other metadata.
RegUses collectDirectRegUses(Function& func);

// Compute and return the output type of the given instruction, ignoring the
// current type of its output Register.
Type outputType(const Instr& instr);

// Compute and return the output type of the given instruction, ignoring the
// current type of its output Register. Uses the `get_op_type` function to get
// the type of its operands - useful for examining possible output types of
// passthrough instructions.
Type outputType(
    const Instr& instr,
    const std::function<Type(std::size_t)>& get_op_type);

// Re-derive all Register types in the given function. Meant to be called after
// SSAify and any optimizations that could refine the output type of an
// instruction.
void reflowTypes(Function& func);
void reflowTypes(Function& func, BasicBlock* start);

// Combine all blocks A and B where A only has B as a successor, B only has A as
// a predecessor, and A and B are distinct blocks (not cycles).  Chains of such
// blocks collapse down into a single block.  Return true if the CFG changed.
//
// Any Phi at the top of B is necessarily trivial and gets collapsed into an
// Assign, as Phis can only live at the start of a block.
bool mergeLinearBlocks(Function& func);

// Remove blocks that aren't reachable from the entry, whether or not they're
// empty. Return true if it changed the graph and false otherwise.
bool removeUnreachableBlocks(Function& func);

// Remove instructions that aren't reachable from the entry. Return true if it
// changed the graph and false otherwise.
bool removeUnreachableInstructions(Function& func);

} // namespace cinderx::jit::hir
