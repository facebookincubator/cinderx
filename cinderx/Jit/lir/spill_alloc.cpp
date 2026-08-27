// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/spill_alloc.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/printer.h"

#include <utility>
#include <vector>

using namespace cinderx::jit::codegen;

namespace cinderx::jit::lir {

namespace {

// An operand should be replaced with its allocated physical location if it is a
// virtual register, or a use linked to one.
bool shouldReplaceOperand(const Operand& operand) {
  return operand.isVreg() || operand.isLinked();
}

// Whether phi copies must be inserted before this instruction (a control
// transfer at the end of a block) rather than at the very end of the block.
bool isBlockTerminator(const Instruction* instr) {
  return instr->isBranch() || isAnyBranch(instr->opcode()) ||
      isTerminator(instr->opcode());
}

} // namespace

SpillAllocator::SpillAllocator(Function* func, int reserved_stack_space)
    : func_{func},
      initial_max_stack_slot_{-reserved_stack_space},
      max_stack_slot_{initial_max_stack_slot_} {
  // Scratch registers are drawn only from the caller-save pool, so no
  // callee-saved register ever needs to be preserved.  The general and double
  // return registers are excluded because handleFramePointerSwitch carries a
  // value across the frame-pointer switch in them: it loads into the return
  // register before the switch and stores from it after, and the fall-through
  // per-instruction rewriting must not reuse that register as scratch on the
  // same instruction and clobber the crossing value.  On x86-64, RDX is
  // additionally left out because PostRegAllocRewrite reserves it (along with
  // RAX, which is the general return register) when lowering division, byte
  // multiplication, and call-return handling.
  PhyRegisterSet gp = CALLER_SAVE_REGS & ALL_GP_REGISTERS;
  gp = gp - PhyRegisterSet(codegen::arch::reg_general_return_loc);
#if defined(CINDER_X86_64)
  gp = gp - PhyRegisterSet(RDX);
#endif
  while (!gp.empty()) {
    gp_scratch_.push_back(gp.getFirst());
    gp.removeFirst();
  }

  PhyRegisterSet fp = CALLER_SAVE_REGS & ALL_VECD_REGISTERS;
  fp = fp - PhyRegisterSet(codegen::arch::reg_double_return_loc);
  while (!fp.empty()) {
    fp_scratch_.push_back(fp.getFirst());
    fp.removeFirst();
  }
}

void SpillAllocator::run() {
  func_->sortBasicBlocks();
  assignSlots();

  // Lower phis and pseudo-terminators before per-instruction rewriting.  The
  // rewriter inserts operand loads right before each instruction, and running
  // it afterwards keeps those loads between the phi copies and the terminator
  // so they don't clobber a value the phi copies just produced.
  resolveControlFlow();

  for (BasicBlock* block : func_->basicBlocks()) {
    // Snapshot the iterators up front: rewriting inserts load/store moves into
    // the list, and we don't want to reprocess those.
    std::vector<instr_iter_t> iters;
    iters.reserve(block->getNumInstrs());
    for (auto it = block->instructions().begin();
         it != block->instructions().end();
         ++it) {
      iters.push_back(it);
    }
    for (instr_iter_t it : iters) {
      rewriteInstr(block, it);
    }
  }

  // Only now that every use has been rewritten (uses of a phi result are linked
  // to the phi's output operand) is it safe to delete the phi instructions.
  removePhis();
}

codegen::PhyRegisterSet SpillAllocator::getChangedRegs() const {
  return changed_regs_;
}

int SpillAllocator::getFrameSize() const {
  return -max_stack_slot_;
}

void SpillAllocator::assignSlots() {
  for (BasicBlock* block : func_->basicBlocks()) {
    for (auto& instr : block->instructions()) {
      Operand* out = instr->output();
      if (!out->isVreg()) {
        continue;
      }
      // Intentionally align every slot to 8 bytes regardless of the value's
      // size.  Uses more stack but avoids alignment issues.
      max_stack_slot_ -= kPointerSize;
      slots_.emplace(out, PhyLocation{max_stack_slot_, out->sizeInBits()});
    }
  }
}

PhyLocation SpillAllocator::slotFor(const Operand* def) const {
  auto iter = slots_.find(def);
  JIT_THROW_IF(
      iter == slots_.end(), "No stack slot assigned for operand {}", *def);
  return iter->second;
}

void SpillAllocator::resetScratch() {
  gp_scratch_idx_ = 0;
  fp_scratch_idx_ = 0;
}

PhyLocation SpillAllocator::takeScratch(bool is_fp) {
  PhyLocation reg;
  if (is_fp) {
    JIT_THROW_IF(
        fp_scratch_idx_ >= fp_scratch_.size(),
        "Ran out of scratch FP registers");
    reg = fp_scratch_[fp_scratch_idx_++];
  } else {
    JIT_THROW_IF(
        gp_scratch_idx_ >= gp_scratch_.size(),
        "Ran out of scratch GP registers");
    reg = gp_scratch_[gp_scratch_idx_++];
  }
  changed_regs_.set(reg);
  return reg;
}

PhyLocation SpillAllocator::loadToScratch(
    BasicBlock* block,
    instr_iter_t iter,
    PhyLocation slot,
    DataType data_type,
    bool is_fp) {
  PhyLocation reg = takeScratch(is_fp);
  block->allocateInstrBefore(
      iter, Opcode::kLoad, OutPhyReg{reg, data_type}, Stk{slot, data_type});
  return reg;
}

void SpillAllocator::rewriteInstr(BasicBlock* block, instr_iter_t iter) {
  Instruction* instr = iter->get();

  if (instr->isPhi()) {
    return; // resolved and removed in resolveControlFlow().
  }
  if (instr->isBind()) {
    rewriteBind(block, iter);
    return;
  }
  if (instr->isCall() || instr->isVarArgCall() || instr->isVectorCallTstate()) {
    rewriteCall(instr);
    return;
  }

  JIT_DCHECK(
      !instr->isLoadPair(),
      "load pair can only be generated after register allocation");

  // Generator frame migration: a Move into the frame-pointer register swaps the
  // frame pointer between the machine stack and the heap-allocated generator
  // data.  Handle values that must cross the switch, then fall through so the
  // Move's own operands are still rewritten.
  if (instr->isMove() && instr->output()->isReg() &&
      instr->output()->getPhyRegister() ==
          codegen::arch::reg_frame_pointer_loc) {
    handleFramePointerSwitch(block, iter);
  }

  resetScratch();

  // Inc/Dec are read-modify-write on input 0, operating in place.  The value's
  // home slot must end up holding the modified value, since later reads (of
  // input 0's def or of the output) read from a slot.  We can't just emit
  // `inc [slot]`: codegen's move-sequence optimizer rewrites the in-place
  // memory operand into a register, leaving the slot stale.  Instead load into
  // a scratch register, modify the register, and store it back to the input's
  // home slot (and the output's slot, if there is one).  The stores are plain
  // moves, so the flags the inc/dec set for a following branch are preserved.
  if ((instr->isInc() || instr->isDec()) && instr->getNumInputs() == 1 &&
      shouldReplaceOperand(*instr->getInput(0))) {
    Operand* in = instr->getInput(0);
    DataType dt = in->dataType();
    PhyLocation in_slot = slotFor(in->getDefine());
    PhyLocation reg = loadToScratch(block, iter, in_slot, dt, in->isFp());

    auto new_in = std::make_unique<Operand>();
    new_in->setPhyRegister(reg);
    new_in->setDataType(dt);
    instr->setInput(0, std::move(new_in));

    auto next_iter = std::next(iter);
    block->allocateInstrBefore(
        next_iter, Opcode::kStore, OutStk{in_slot, dt}, PhyReg{reg, dt});

    Operand* out = instr->output();
    if (out->isVreg()) {
      PhyLocation out_slot = slotFor(out);
      block->allocateInstrBefore(
          next_iter, Opcode::kStore, OutStk{out_slot, dt}, PhyReg{reg, dt});
      out->setPhyRegOrStackSlot(out_slot);
    }
    return;
  }

  // Rewrite inputs.
  for (size_t i = 0, n = instr->getNumInputs(); i < n; i++) {
    Operand* input = instr->getInput(i);
    if (input->isInd()) {
      rewriteIndirect(block, iter, input->getMemoryIndirect());
      continue;
    }
    if (input->isNone() || !shouldReplaceOperand(*input)) {
      continue;
    }
    DataType dt = input->dataType();
    PhyLocation slot = slotFor(input->getDefine());
    auto new_input = std::make_unique<Operand>();
    new_input->setDataType(dt);
    if (instr->getInputPhyRegUse(i)) {
      new_input->setPhyRegister(
          loadToScratch(block, iter, slot, dt, input->isFp()));
    } else {
      new_input->setPhyRegOrStackSlot(slot);
    }
    bool spilled = new_input->isStack();
    instr->setInput(i, std::move(new_input));

    // Spilling the source of a register copy turns it into a load.
    if (spilled && instr->isMove()) {
      instr->setOpcode(Opcode::kLoad);
    }
  }

  // Rewrite output.
  Operand* out = instr->output();
  if (out->isInd()) {
    rewriteIndirect(block, iter, out->getMemoryIndirect());
  } else if (out->isVreg()) {
    DataType dt = out->dataType();
    PhyLocation slot = slotFor(out);
    if (instr->getOutputPhyRegUse()) {
      PhyLocation reg = takeScratch(out->isFp());
      out->setPhyRegister(reg);
      block->allocateInstrBefore(
          std::next(iter), Opcode::kStore, OutStk{slot, dt}, PhyReg{reg, dt});
    } else {
      out->setPhyRegOrStackSlot(slot);
      // Spilling the destination of a register copy turns it into a store.
      if (out->isStack() && instr->isMove()) {
        instr->setOpcode(Opcode::kStore);
      }
    }
  }
}

void SpillAllocator::rewriteCall(Instruction* instr) {
  // Leave call operands in their stack slots (or as immediates).
  // PostRegAllocRewrite moves them into argument registers and moves the return
  // value out of the return register, honoring the calling convention.
  for (size_t i = 0, n = instr->getNumInputs(); i < n; i++) {
    Operand* input = instr->getInput(i);
    if (input->isInd() || input->isNone() || !shouldReplaceOperand(*input)) {
      continue;
    }
    DataType dt = input->dataType();
    auto new_input = std::make_unique<Operand>();
    new_input->setDataType(dt);
    new_input->setPhyRegOrStackSlot(slotFor(input->getDefine()));
    instr->setInput(i, std::move(new_input));
  }

  Operand* out = instr->output();
  if (out->isVreg()) {
    out->setPhyRegOrStackSlot(slotFor(out));
  }
}

void SpillAllocator::rewriteBind(BasicBlock* /* block */, instr_iter_t iter) {
  Instruction* instr = iter->get();
  Operand* out = instr->output();
  JIT_THROW_IF(
      !out->isVreg(),
      "kBind output must be a virtual register, have '{}'",
      *out);
  JIT_THROW_IF(
      instr->getNumInputs() != 1 || !instr->getInput(0)->isReg(),
      "kBind must bind exactly one physical register, have '{}'",
      *instr);

  // A kBind associates a value with an incoming physical register but emits no
  // code.  Turn it into a store of that register into the value's home slot.
  constexpr DataType dt = DataType::k64bit;
  instr->getInput(0)->setDataType(dt);
  instr->setOpcode(Opcode::kStore);
  out->setDataType(dt);
  out->setPhyRegOrStackSlot(slotFor(out));
}

void SpillAllocator::handleFramePointerSwitch(
    BasicBlock* block,
    instr_iter_t iter) {
  Instruction* move = iter->get();

  // Reverse switch: `Move fp, [fp + originalFramePointer]` restores the machine
  // stack frame in the epilogue.  The return value (the exit phi) was written
  // in the heap frame that is about to be abandoned, but the epilogue keeps
  // reading it from its slot after the switch -- either directly (EpilogueEnd)
  // or through an intermediate step such as stripping a deferred-refcount tag
  // on free-threaded builds.  Copy that slot across the switch: load it while
  // the heap frame is still active, then store it back to the same slot in the
  // restored stack frame so the later reads are correct.  The first value read
  // after the switch that was defined before it is the exit phi.
  if (move->getInput(0)->isInd()) {
    for (auto fwd = std::next(iter); fwd != block->instructions().end();
         ++fwd) {
      Instruction* next = fwd->get();
      const Operand* crossing = nullptr;
      for (size_t i = 0, n = next->getNumInputs(); i < n; i++) {
        Operand* in = next->getInput(i);
        if (!in->isInd() && !in->isNone() && shouldReplaceOperand(*in)) {
          crossing = in;
          break;
        }
      }
      if (crossing == nullptr) {
        if (next->isEpilogueEnd()) {
          break;
        }
        continue;
      }
      DataType dt = crossing->dataType();
      PhyLocation slot = slotFor(crossing->getDefine());
      PhyLocation reg = crossing->isFp()
          ? codegen::arch::reg_double_return_loc
          : codegen::arch::reg_general_return_loc;
      block->allocateInstrBefore(
          iter, Opcode::kLoad, OutPhyReg{reg, dt}, Stk{slot, dt});
      block->allocateInstrBefore(
          std::next(iter), Opcode::kStore, OutStk{slot, dt}, PhyReg{reg, dt});
      changed_regs_.set(reg);
      return;
    }
    return;
  }

  // Forward switch: `Move fp, <footer vreg>`.  Walk back to the frame-setup
  // call (e.g. allocateAndLinkGenAndInterpreterFrame) whose result is needed
  // after the switch.  Its result is still in the return register, so re-store
  // it into its home slot, which now lives in the heap-allocated generator
  // frame.
  for (auto back = iter; back != block->instructions().begin();) {
    --back;
    Instruction* prev = back->get();
    if (!prev->isCall() && !prev->isVarArgCall() &&
        !prev->isVectorCallTstate()) {
      continue;
    }
    Operand* out = prev->output();
    if (out->isStack()) {
      DataType dt = out->dataType();
      block->allocateInstrBefore(
          std::next(iter),
          Opcode::kStore,
          OutStk{out->getStackSlot(), dt},
          PhyReg{codegen::arch::reg_general_return_loc, dt});
      changed_regs_.set(codegen::arch::reg_general_return_loc);
    }
    return;
  }
}

void SpillAllocator::rewriteIndirect(
    BasicBlock* block,
    instr_iter_t iter,
    MemoryIndirect* indirect) {
  Operand* base = indirect->getBaseRegOperand();
  JIT_THROW_IF(base == nullptr, "Memory indirect must have a base operand");
  PhyLocation base_reg = shouldReplaceOperand(*base)
      ? loadToScratch(
            block, iter, slotFor(base->getDefine()), DataType::k64bit, false)
      : base->getPhyRegister();

  Operand* index = indirect->getIndexRegOperand();
  PhyLocation index_reg = PhyLocation::REG_INVALID;
  if (index != nullptr) {
    index_reg = shouldReplaceOperand(*index)
        ? loadToScratch(
              block, iter, slotFor(index->getDefine()), DataType::k64bit, false)
        : index->getPhyRegister();
  }

  indirect->setMemoryIndirect(
      base_reg, index_reg, indirect->getMultiplier(), indirect->getOffset());
}

void SpillAllocator::resolveControlFlow() {
  auto& blocks = func_->basicBlocks();

  for (BasicBlock* pred : blocks) {
    auto& succs = pred->successors();
    if (succs.empty()) {
      continue;
    }

    Instruction* last = pred->getLastInstr();
    std::optional<Opcode> last_op =
        last != nullptr ? std::make_optional(last->opcode()) : std::nullopt;

    // Yield blocks (ending with BranchToYieldExit) carry a second, "phantom"
    // resume successor used only for liveness in the linear scan allocator.
    // With per-slot spilling, liveness is irrelevant, so treat the block as
    // unconditional and drop the phantom edge.
    bool yield_with_resume =
        last_op == Opcode::kBranchToYieldExit && succs.size() == 2;

    if (succs.size() == 1 || yield_with_resume) {
      emitPhiCopies(pred, succs.front());

      // kReturn and kBranchToYieldExit are pseudo-terminators; remove them so
      // PostRegAllocRewrite can insert a real branch to the successor.
      if (last_op == Opcode::kReturn || last_op == Opcode::kBranchToYieldExit) {
        pred->removeInstr(pred->getLastInstrIter());
      }
      if (yield_with_resume) {
        succs.pop_back();
      }
    } else {
      // Conditional branch.  Each phi output has its own slot, so emitting the
      // copies for both successors at the end of the predecessor is safe: a
      // successor only ever reads the slots for its own phis.
      for (BasicBlock* succ : succs) {
        emitPhiCopies(pred, succ);
      }
    }
  }
}

void SpillAllocator::removePhis() {
  for (BasicBlock* block : func_->basicBlocks()) {
    auto& instrs = block->instructions();
    for (auto it = instrs.begin(); it != instrs.end();) {
      if ((*it)->isPhi()) {
        it = instrs.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void SpillAllocator::emitPhiCopies(BasicBlock* pred, BasicBlock* succ) {
  instr_iter_t insert_at = pred->instructions().end();
  Instruction* last = pred->getLastInstr();
  if (last != nullptr && isBlockTerminator(last)) {
    insert_at = pred->getLastInstrIter();
  }

  succ->foreachPhiInstr([&](const Instruction* phi) {
    const Operand* src = phi->getOperandByPredecessor(pred);
    if (src == nullptr) {
      return;
    }

    PhyLocation dst = slotFor(phi->output());
    DataType dt = phi->output()->dataType();

    resetScratch();
    if (shouldReplaceOperand(*src)) {
      PhyLocation reg = loadToScratch(
          pred,
          insert_at,
          slotFor(src->getDefine()),
          dt,
          phi->output()->isFp());
      pred->allocateInstrBefore(
          insert_at, Opcode::kStore, OutStk{dst, dt}, PhyReg{reg, dt});
    } else if (src->isImm()) {
      pred->allocateInstrBefore(
          insert_at,
          Opcode::kStore,
          OutStk{dst, dt},
          Imm{src->getConstant(), dt});
    } else if (src->isReg()) {
      pred->allocateInstrBefore(
          insert_at,
          Opcode::kStore,
          OutStk{dst, dt},
          PhyReg{src->getPhyRegister(), dt});
    } else {
      JIT_THROW("Unsupported phi operand for spill allocation: {}", *src);
    }
  });
}

} // namespace cinderx::jit::lir
