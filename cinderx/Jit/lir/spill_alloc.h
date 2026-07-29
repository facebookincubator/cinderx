// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/containers.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/regalloc.h"

#include <vector>

namespace cinderx::jit::lir {

// A deliberately naive register allocator that spills every virtual register to
// its own stack slot.
//
// The goal is not to produce fast code but to exercise the rest of the JIT
// pipeline (LIR lowering, post-alloc rewriting, and codegen) without the
// complexity of the optimizing linear scan allocator.
//
// The strategy is:
//   1. Give every virtual register a home stack slot, which it keeps for the
//      whole function.  A value defined in one block and used in another is
//      simply read back from the same slot, so no cross-block bookkeeping (live
//      intervals, interval splitting, edge resolution) is required.
//   2. Rewrite each instruction so it only refers to physical locations.  For
//      operands that must live in a register (per
//      Instruction::getInputPhyRegUse / getOutputPhyRegUse), a value is loaded
//      from its slot into a scratch register right before the instruction and,
//      for outputs, stored back right after.  Everything else is left in its
//      stack slot.
//   3. Lower phis to copies on the incoming edges and drop the phi nodes.
//
// Because values never stay in a register across instruction boundaries, the
// caller-save registers are always free at a call and every live value is
// already in memory at a yield, so calls and yields need no special spilling.
// Scratch registers are drawn only from the caller-save pool, so no
// callee-saved registers ever need to be preserved.
class SpillAllocator : public RegisterAllocator {
 public:
  explicit SpillAllocator(Function* func, int reserved_stack_space = 0);

  void run() override;

  codegen::PhyRegisterSet getChangedRegs() const override;

  int getFrameSize() const override;

 private:
  // Assign a home stack slot to every virtual register defined in the function.
  void assignSlots();

  // Get the home stack slot for the value defined by `def`.
  PhyLocation slotFor(const Operand* def) const;

  // Rewrite a single instruction so it only refers to physical locations,
  // inserting loads/stores through scratch registers as needed.
  void rewriteInstr(BasicBlock* block, instr_iter_t iter);

  // Rewrite a kCall/kVarArgCall/kVectorCallTstate: operands stay in their slots
  // and PostRegAllocRewrite handles the calling convention.
  void rewriteCall(Instruction* instr);

  // Turn a kBind into a store of its bound physical register into the bound
  // value's home slot.
  void rewriteBind(BasicBlock* block, instr_iter_t iter);

  // Handle a generator frame migration `Move` into the frame-pointer register
  // (`iter`).  There are two kinds:
  //  - Forward (`Move fp, <footer vreg>`): the machine-stack frame is swapped
  //    for the heap generator frame.  The result of the preceding frame-setup
  //    call was spilled to the now-abandoned stack frame (and is stale in the
  //    heap copy), so re-store it -- still live in the return register -- into
  //    its slot after the switch.
  //  - Reverse (`Move fp, [fp + originalFramePointer]`): the epilogue restores
  //    the original stack frame.  The return value (exit phi) still lives in a
  //    heap-frame slot but is read after the switch, so copy that slot across
  //    the switch (load while the heap frame is active, store back afterwards).
  void handleFramePointerSwitch(BasicBlock* block, instr_iter_t iter);

  // Rewrite the base/index of a memory-indirect operand into scratch registers.
  void rewriteIndirect(
      BasicBlock* block,
      instr_iter_t iter,
      MemoryIndirect* indirect);

  // Lower phis to copies on the incoming edges and strip pseudo-terminators
  // (kReturn / kBranchToYieldExit) so the CFG is ready for PostRegAllocRewrite.
  // Leaves the phi instructions in place so uses of phi results can still be
  // resolved; removePhis() deletes them afterwards.
  void resolveControlFlow();

  // Delete phi instructions once all their uses have been rewritten.
  void removePhis();

  // Emit the phi copies for the edge from `pred` to `succ` at the end of
  // `pred`.
  void emitPhiCopies(BasicBlock* pred, BasicBlock* succ);

  // Reset the per-instruction scratch register counters.
  void resetScratch();

  // Take the next free scratch register of the appropriate kind, recording it
  // as a changed register.
  PhyLocation takeScratch(bool is_fp);

  // Load the value at `slot` into a fresh scratch register before `iter`.
  PhyLocation loadToScratch(
      BasicBlock* block,
      instr_iter_t iter,
      PhyLocation slot,
      DataType data_type,
      bool is_fp);

  Function* func_;

  int initial_max_stack_slot_;
  int max_stack_slot_;

  // Home stack slot for each virtual register, keyed by its defining operand.
  UnorderedMap<const Operand*, PhyLocation> slots_;

  codegen::PhyRegisterSet changed_regs_;

  // Caller-save registers usable as per-instruction scratch.
  std::vector<PhyLocation> gp_scratch_;
  std::vector<PhyLocation> fp_scratch_;

  // Indices into the scratch pools, reset for each instruction.
  size_t gp_scratch_idx_{0};
  size_t fp_scratch_idx_{0};
};

} // namespace cinderx::jit::lir
