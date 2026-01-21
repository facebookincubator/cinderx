// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/regalloc.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/lir/printer.h"

#include <algorithm>
#include <iterator>
#include <utility>

using namespace jit::codegen;

#define TRACE(...) JIT_LOGIF(getConfig().log.debug_regalloc, __VA_ARGS__)

namespace jit::lir {

namespace {

struct LiveRangeCompare {
  // Support searching by LIRLocation.
  using is_transparent = void;

  bool operator()(const LiveRange& lhs, LIRLocation rhs) const {
    return lhs.start < rhs;
  }

  bool operator()(LIRLocation lhs, const LiveRange& rhs) const {
    return lhs < rhs.start;
  }
};

void markDisallowedRegisters(std::vector<LIRLocation>& locs) {
  auto disallowed_registers = DISALLOWED_REGISTERS;
  while (!disallowed_registers.Empty()) {
    auto reg = disallowed_registers.GetFirst();
    disallowed_registers.RemoveFirst();

    locs[reg.loc] = START_LOCATION;
  }
}

// Check if an operand should be replaced with a new one by the register
// allocator.
bool shouldReplaceOperand(const OperandBase& operand) {
  // Linked operands are always replaced with new Operand instances.
  return operand.isVreg() || operand.isLinked();
}

} // namespace

RegallocBlockState::RegallocBlockState(
    const BasicBlock* b,
    int index,
    Instruction* instr)
    : bb(b), block_start_index(index), block_first_instr(instr) {}

LiveRange::LiveRange(LIRLocation s, LIRLocation e) : start{s}, end{e} {
  JIT_CHECK(s < e, "Invalid live range: {}:{}", s, e);
}

bool LiveRange::isInRange(const LIRLocation& loc) const {
  return loc >= start && loc < end;
}

bool LiveRange::intersectsWith(const LiveRange& range) const {
  const LiveRange* a = this;
  const LiveRange* b = &range;

  if (b->start < a->start) {
    std::swap<const LiveRange*>(a, b);
  }

  return b->start < a->end;
}

LiveInterval::LiveInterval(const Operand* operand) : operand{operand} {
  allocated_loc = PhyLocation{PhyLocation::REG_INVALID, operand->sizeInBits()};
}

void LiveInterval::addRange(LiveRange range) {
  constexpr int kInitRangeSize = 8;
  if (ranges.empty()) {
    ranges.reserve(kInitRangeSize);
    ranges.push_back(std::move(range));
    return;
  }

  auto& start = range.start;
  auto& end = range.end;

  auto iter =
      std::lower_bound(ranges.begin(), ranges.end(), start, LiveRangeCompare());

  // Can't use INVALID_LOCATION here, use a different value.
  constexpr int kRemovedRange = std::numeric_limits<int>::min();

  auto cur_iter = iter;
  // check if can merge with *cur_iter
  while (cur_iter != ranges.end() && end >= cur_iter->start) {
    end = std::max(end, cur_iter->end);
    cur_iter->start = kRemovedRange;
    ++cur_iter;
  }

  // check if we can merge with iter - 1
  bool merged = false;
  if (iter != ranges.begin()) {
    auto prev_iter = std::prev(iter);
    if (start <= prev_iter->end) {
      prev_iter->end = std::max(end, prev_iter->end);
      merged = true;
    }
  }

  if (!merged) {
    if (iter != ranges.end() && iter->start == kRemovedRange) {
      *iter = std::move(range);
    } else {
      ranges.insert(iter, std::move(range));
    }
  }

  std::erase_if(ranges, [](const LiveRange& range) {
    return range.start == kRemovedRange;
  });
}

void LiveInterval::setFrom(LIRLocation loc) {
  if (ranges.empty()) {
    return;
  }

  // We need to care about only the first (earliest in time) range here.
  // This is because the function is only used for setting the from point
  // of a range when a def of a vreg is encountered. The range should be
  // most recently inserted when uses of the same vreg were encountered, and
  // due to the fact that the basic blocks and the instructions are iterated
  // in reverse order, it should be always the first element.
  // For the case of loop, the above may not be always true, but it will be
  // handled separately.
  auto iter = ranges.begin();

  if (loc >= iter->end) {
    ranges.erase(iter);
  } else {
    iter->start = std::max(loc, iter->start);
  }
}

LIRLocation LiveInterval::startLocation() const {
  JIT_CHECK(
      !ranges.empty(), "Cannot get start location for an empty interval.");
  return ranges.begin()->start;
}

LIRLocation LiveInterval::endLocation() const {
  JIT_CHECK(!ranges.empty(), "Cannot get end location for an empty interval.");
  return ranges.rbegin()->end;
}

bool LiveInterval::covers(LIRLocation loc) const {
  auto iter =
      std::upper_bound(ranges.begin(), ranges.end(), loc, LiveRangeCompare());

  if (iter == ranges.begin()) {
    return false;
  }

  --iter;

  return iter->end > loc;
}

bool LiveInterval::isEmpty() const {
  return ranges.empty();
}

LIRLocation LiveInterval::intersectWith(const LiveRange& range) const {
  if (ranges.empty()) {
    return INVALID_LOCATION;
  }

  auto iter = std::lower_bound(
      ranges.begin(), ranges.end(), range.start, LiveRangeCompare());

  // iter is the first candidate that starts at or after range.start. The
  // intersection could be with the previous candidate, so check that first.
  if (iter != ranges.begin() && std::prev(iter)->intersectsWith(range)) {
    return range.start;
  }

  if (iter != ranges.end() && iter->intersectsWith(range)) {
    return iter->start;
  }

  return INVALID_LOCATION;
}

LIRLocation LiveInterval::intersectWith(const LiveInterval& interval) const {
  const auto* a = this;
  const auto* b = &interval;

  if (a->ranges.size() > b->ranges.size()) {
    std::swap(a, b);
  }

  for (auto& range : a->ranges) {
    auto loc = b->intersectWith(range);
    if (loc != INVALID_LOCATION) {
      return loc;
    }
  }

  return INVALID_LOCATION;
}

std::unique_ptr<LiveInterval> LiveInterval::splitAt(LIRLocation loc) {
  JIT_CHECK(!fixed, "Trying to split fixed interval {} at {}", *this, loc);

  if (loc <= startLocation() || loc >= endLocation()) {
    return nullptr;
  }

  auto new_interval = std::make_unique<LiveInterval>(operand);
  new_interval->allocated_loc = allocated_loc;

  auto iter =
      std::lower_bound(ranges.begin(), ranges.end(), loc, LiveRangeCompare());

  --iter;

  // if loc is within the range pointed by iter
  if (loc < iter->end) {
    // need to split the current range
    new_interval->ranges.emplace_back(loc, iter->end);
    iter->end = loc;
  }

  ++iter;

  new_interval->ranges.insert(new_interval->ranges.end(), iter, ranges.end());
  ranges.erase(iter, ranges.end());

  return new_interval;
}

void LiveInterval::allocateTo(PhyLocation loc) {
  JIT_CHECK(
      allocated_loc.bitSize == loc.bitSize,
      "Trying to change size of live interval: {} -> {}, with location {} -> "
      "{}, for operand {}",
      allocated_loc.bitSize,
      loc.bitSize,
      allocated_loc,
      loc,
      *operand);
  allocated_loc = loc;
}

bool LiveInterval::isAllocated() const {
  return allocated_loc != PhyLocation::REG_INVALID;
}

bool LiveInterval::isRegisterAllocated() const {
  return isAllocated() && allocated_loc.is_register();
}

LinearScanAllocator::LinearScanAllocator(
    Function* func,
    int reserved_stack_space)
    : func_{func},
      initial_max_stack_slot_{-reserved_stack_space},
      max_stack_slot_{initial_max_stack_slot_} {}

void LinearScanAllocator::run() {
  TRACE("Starting register allocation");

  func_->sortBasicBlocks();

  calculateLiveIntervals();
  linearScan();
  rewriteLIR();
  resolveEdges();
}

codegen::PhyRegisterSet LinearScanAllocator::getChangedRegs() const {
  return changed_regs_;
}

int LinearScanAllocator::getFrameSize() const {
  return -max_stack_slot_;
}

int LinearScanAllocator::initialYieldSpillSize() const {
  JIT_CHECK(
      initial_yield_spill_size_ != -1,
      "Don't have InitialYield spill size yet");

  return initial_yield_spill_size_;
}

bool LinearScanAllocator::isPredefinedUsed(const Operand* operand) const {
  auto& block = func_->basicblocks()[0];

  for (auto& succ : block->successors()) {
    if (map_get(regalloc_blocks_, succ).livein.contains(operand)) {
      return true;
    }
  }

  return false;
}

const LinearScanAllocator::IntervalMap& LinearScanAllocator::intervalMap()
    const {
  return intervals_;
}

const LinearScanAllocator::IntervalList& LinearScanAllocator::intervalList()
    const {
  return allocated_;
}

LiveInterval& LinearScanAllocator::getInterval(const Operand* operand) {
  return intervals_.emplace(operand, operand).first->second;
}

void LinearScanAllocator::calculateLiveIntervals() {
  const auto& basic_blocks = func_->basicblocks();

  // This table maps loop headers to all their loop ends. A loop end basic
  // block is the last block of a loop starting at the loop header.
  // The key is the pointer to the loop header and the value std::vector<int>
  // is a vector of the block ids of all the associated loop ends.
  UnorderedMap<const BasicBlock*, std::vector<int>> loop_ends;
  UnorderedSet<const Operand*> seen_outputs;

  int total_instrs = 0;
  for (auto& bb : basic_blocks) {
    total_instrs += bb->getNumInstrs();
  }
  int total_ids = total_instrs * 2 + basic_blocks.size();

  UnorderedSet<const BasicBlock*> visited_blocks;
  for (auto iter = basic_blocks.rbegin(); iter != basic_blocks.rend(); ++iter) {
    BasicBlock* bb = *iter;

    // bb_start_id and bb_end_id do not point to any instructions.
    // each instrution is associated to two ids, where the first id
    // is for using its inputs, and the second id is for defining
    // its output.

    // Basic block M
    // x   <- bb_start_id
    // x + 1  instructions1
    // x + 2
    // x + 3  instructions2
    // x + 4
    // ...
    // x + 2N - 1  instructionsN
    // x + 2N
    // basic block M + 1
    // x + 2N + 1   <- bb_end_id of bb M, bb_start_id of block M + 1
    constexpr int kIdsPerInstr = 2;
    auto bb_end_id = total_ids;
    auto bb_instrs = bb->getNumInstrs() * kIdsPerInstr;
    total_ids -= bb_instrs;
    total_ids--;
    auto bb_start_id = total_ids;

    auto lir_bb_iter =
        regalloc_blocks_
            .emplace(
                std::piecewise_construct,
                std::forward_as_tuple(bb),
                std::forward_as_tuple(bb, bb_start_id, bb->getFirstInstr()))
            .first;

    auto& successors = bb->successors();

    UnorderedSet<const Operand*> live;

    for (BasicBlock* succ : successors) {
      // each successor's livein is live
      auto live_iter = regalloc_blocks_.find(succ);
      if (live_iter != regalloc_blocks_.end()) {
        auto& livein = live_iter->second.livein;
        live.insert(livein.begin(), livein.end());
      }

      // each successor's phi inputs are live
      succ->foreachPhiInstr([&](const Instruction* instr) {
        auto opnd = instr->getOperandByPredecessor(bb)->getDefine();
        live.insert(opnd);
      });
    }

    for (const Operand* live_opnd : live) {
      getInterval(live_opnd).addRange({bb_start_id, bb_end_id});
    }

    int instr_id = bb_end_id - kIdsPerInstr;
    auto& instrs = bb->instructions();
    for (auto instr_iter = instrs.rbegin(); instr_iter != instrs.rend();
         ++instr_iter, instr_id -= kIdsPerInstr) {
      auto instr = instr_iter->get();
      auto instr_opcode = instr->opcode();
      if (instr_opcode == Instruction::kPhi) {
        // ignore phi instructions
        continue;
      }

      // output
      auto output_opnd = instr->output();
      if (output_opnd->isVreg()) {
        if (kPyDebug) {
          auto inserted = seen_outputs.insert(output_opnd).second;
          JIT_CHECK(
              inserted,
              "LIR not in SSA form, output {} defined twice",
              *output_opnd);
        }
        getInterval(output_opnd).setFrom(instr_id + 1);
        live.erase(output_opnd);

        if (instr->getOutputPhyRegUse()) {
          vreg_phy_uses_[output_opnd].emplace(instr_id + 1);
        }
      }

      auto register_input = [&](const OperandBase* operand, bool reguse) {
        auto def = operand->getDefine();

        auto pair = intervals_.emplace(def, def);

        bool live_across = operand->instr()->inputsLiveAcross();
        int range_end = live_across ? instr_id + kIdsPerInstr : instr_id + 1;
        pair.first->second.addRange({bb_start_id, range_end});

        // if the def is not live before, record the last use
        if (!live.count(def) && operand->isLinked()) {
          vreg_last_use_[def].emplace(
              static_cast<const LinkedOperand*>(operand), instr_id);
        }

        live.insert(def);
        if (reguse) {
          vreg_phy_uses_[def].emplace(instr_id);
          if (live_across) {
            // Codegen for this instruction is expecting to be able to read its
            // input registers after defining its output, so the inputs must
            // also be in registers at the "define output" id.
            vreg_phy_uses_[def].emplace(instr_id + 1);
          }
        }
      };

      auto visit_indirect = [&](const OperandBase* operand) {
        auto indirect = operand->getMemoryIndirect();
        auto base = indirect->getBaseRegOperand();
        if (base->isVreg()) {
          register_input(base, true);
        }

        auto index = indirect->getIndexRegOperand();
        if (index != nullptr && index->isVreg()) {
          register_input(index, true);
        }
      };

      // if output is a memory indirect, the base and index registers
      // should be considered as inputs.
      if (output_opnd->isInd()) {
        visit_indirect(output_opnd);
      }

      // inputs
      for (size_t i = 0; i < instr->getNumInputs(); i++) {
        const OperandBase* opnd = instr->getInput(i);
        if (!opnd->isVreg() && !opnd->isInd()) {
          continue;
        }

        if (opnd->isInd()) {
          visit_indirect(opnd);
          continue;
        }

        register_input(opnd, instr->getInputPhyRegUse(i));
      }

      if (instr_opcode == Instruction::kCall ||
          instr_opcode == Instruction::kVarArgCall ||
          instr_opcode == Instruction::kVectorCall) {
        reserveCallerSaveRegisters(instr_id);
      }

#if defined(CINDER_X86_64)
      if ((instr_opcode == Instruction::kMul) &&
          (instr->getInput(0)->dataType() == OperandBase::k8bit)) {
        // see rewriteByteMultiply
        reserveRegisters(instr_id, PhyRegisterSet(RAX));
      } else if (
          instr_opcode == Instruction::kDiv ||
          instr_opcode == Instruction::kDivUn) {
        PhyRegisterSet reserved(RAX);

        if (instr->getInput(1)->dataType() != OperandBase::k8bit) {
          reserved = reserved | RDX;
        }

        reserveRegisters(instr_id, reserved);
      }
#endif

      if (instr->isAnyYield()) {
        spillRegistersForYield(instr_id);
      }

      if (instr_opcode == Instruction::kBind) {
        auto& interval = getInterval(instr->output());
        interval.allocateTo(instr->getInput(0)->getPhyRegister());
      }
    }
    // From the original paper:
    /*
       Phi functions are not processed during this iteration of operations,
       instead they are iterated separately. Because the live range of a phi
       function starts at the beginning of the block, it is not necessary to
       shorten the range for its output operand. The operand is only removed
       from the set of live registers. The input operands of the phi function
       are not handled here, because this is done independently when the
       different predecessors are processed. Thus, neither an input operand nor
       the output operand of a phi function is live at the beginning of the phi
       function's block.
    */
    bb->foreachPhiInstr(
        [&live](const Instruction* phi) { live.erase(phi->output()); });

    auto loop_iter = loop_ends.find(bb);
    if (loop_iter != loop_ends.end()) {
      for (auto& loop_end_id : loop_iter->second) {
        for (auto& opnd : live) {
          LiveRange loop_range(bb_start_id, loop_end_id);
          getInterval(opnd).addRange(loop_range);
          // if the last use is in a loop, it is not a real last use
          auto opnd_iter = vreg_last_use_.find(opnd);
          if (opnd_iter == vreg_last_use_.end()) {
            continue;
          }
          auto& uses = opnd_iter->second;
          for (auto use_iter = uses.begin(); use_iter != uses.end();) {
            LIRLocation use_loc = use_iter->second;
            if (loop_range.isInRange(use_loc)) {
              use_iter = uses.erase(use_iter);
              continue;
            }

            ++use_iter;
          }
        }
      }
    }

    lir_bb_iter->second.livein = std::move(live);

    // record a loop end
    for (auto& succ : bb->successors()) {
      if (visited_blocks.contains(bb)) {
        continue;
      }

      loop_ends[succ].push_back(bb_start_id + bb_instrs + 1);
    }

    visited_blocks.insert(bb);
  }
}

void LinearScanAllocator::spillRegistersForYield(int instr_id) {
  reserveRegisters(instr_id, INIT_REGISTERS);
}

void LinearScanAllocator::computeInitialYieldSpillSize(
    const UnorderedMap<const Operand*, const LiveInterval*>& mapping) {
  JIT_CHECK(
      initial_yield_spill_size_ == -1,
      "Already computed InitialYield spill size");

  for (auto& pair : mapping) {
    const LiveInterval* interval = pair.second;
    if (interval->allocated_loc.is_register()) {
      continue;
    }
    initial_yield_spill_size_ =
        std::max(initial_yield_spill_size_, -interval->allocated_loc.loc);
  }
}

void LinearScanAllocator::reserveCallerSaveRegisters(int instr_id) {
  reserveRegisters(instr_id, CALLER_SAVE_REGS);
}

void LinearScanAllocator::reserveRegisters(
    int instr_id,
    PhyRegisterSet phy_regs) {
  static const UnorderedStablePointerMap<PhyLocation, Operand> vregs = []() {
    UnorderedStablePointerMap<PhyLocation, Operand> result;
    PhyRegisterSet phy_regs = ALL_REGISTERS;
    while (!phy_regs.Empty()) {
      PhyLocation phy_reg = phy_regs.GetFirst();
      phy_regs.RemoveFirst();

      auto& inserted_operand = result.emplace(phy_reg, nullptr).first->second;
      inserted_operand.setPhyRegister(phy_reg);
      inserted_operand.setDataType(
          phy_reg.is_fp_register() ? DataType::kDouble : DataType::k64bit);
    }
    return result;
  }();

  while (!phy_regs.Empty()) {
    PhyLocation reg = phy_regs.GetFirst();
    phy_regs.RemoveFirst();

    const Operand* vreg = &(vregs.at(reg));
    LiveInterval& interval = getInterval(vreg);

    // add a range at the very beginning of the function so that the fixed
    // intervals will be added to active/inactive interval set before any
    // other intervals.
    if (interval.ranges.empty()) {
      interval.addRange({-1, 0});
    }

    interval.addRange({instr_id, instr_id + 1});
    interval.allocateTo(reg);
    interval.fixed = true;

    vreg_phy_uses_[vreg].emplace(instr_id);
  }
}

void LinearScanAllocator::linearScan() {
  for (auto& vi : intervals_) {
    if (vi.second.isEmpty()) {
      continue;
    }
    auto new_interval = std::make_unique<LiveInterval>(vi.second);

    // save the last use location of a virtual register
    vreg_global_last_use_.emplace(vi.first, new_interval->endLocation());

    // All the LiveInterval objects will end up in allocated_, so putting them
    // to allocated_ now even if they are currently not allocated.  All the
    // intervals are guaranteed to be allocated at the end of this function.
    TRACE("Queuing interval {} for allocation", *new_interval);
    allocated_.emplace_back(std::move(new_interval));
  }

  UnorderedSet<LiveInterval*> active;
  UnorderedSet<LiveInterval*> inactive;

  struct LiveIntervalPtrEndLess {
    bool operator()(const LiveInterval* lhs, const LiveInterval* rhs) const {
      const auto& lhs_end = lhs->endLocation();
      const auto& rhs_end = rhs->endLocation();

      if (lhs_end == rhs_end) {
        return lhs->operand < rhs->operand;
      }
      return lhs_end < rhs_end;
    }
  };

  OrderedSet<LiveInterval*, LiveIntervalPtrEndLess> stack_intervals;

  UnhandledQueue unhandled;
  for (auto& interval : allocated_) {
    unhandled.push(interval.get());
  }

  while (!unhandled.empty()) {
    auto current = unhandled.top();
    unhandled.pop();

    auto position = current->startLocation();

    // Return no longer needed stack slots to the allocator.
    for (LiveInterval* interval : stack_intervals) {
      auto operand = interval->operand;
      auto iter = vreg_global_last_use_.find(operand);
      if (iter != vreg_global_last_use_.end() && iter->second <= position) {
        freeStackSlot(operand);
      }
    }
    stack_intervals.clear();

    // Process active intervals, updating to inactive.
    for (auto iter = active.begin(); iter != active.end();) {
      auto interval = *iter;
      if (interval->endLocation() <= position) {
        iter = active.erase(iter);
      } else if (!interval->covers(position)) {
        inactive.insert(interval);
        iter = active.erase(iter);
      } else {
        ++iter;
      }
    }

    // Process inactive intervals, updating to active.
    for (auto iter = inactive.begin(); iter != inactive.end();) {
      auto interval = *iter;
      if (interval->endLocation() <= position) {
        iter = inactive.erase(iter);
      } else if (interval->covers(position)) {
        active.insert(interval);
        iter = inactive.erase(iter);
      } else {
        ++iter;
      }
    }

    if (!tryAllocateFreeReg(current, active, inactive, unhandled)) {
      allocateBlockedReg(current, active, inactive, unhandled);
    }

    if (current->isRegisterAllocated()) {
      changed_regs_.Set(current->allocated_loc);
      active.insert(current);
    } else {
      stack_intervals.emplace(current);
    }
  }

  std::sort(
      allocated_.begin(),
      allocated_.end(),
      [](const auto& lhs, const auto& rhs) {
        return LiveIntervalPtrGreater{}(rhs.get(), lhs.get());
      });
}

bool LinearScanAllocator::tryAllocateFreeReg(
    LiveInterval* current,
    UnorderedSet<LiveInterval*>& active,
    UnorderedSet<LiveInterval*>& inactive,
    UnhandledQueue& unhandled) {
  if (current->fixed) {
    return true;
  }

  // XXX: Feel that we may not need to calculate freeUntilPos every time. Will
  // think about optimizations in the future.
  std::vector<LIRLocation> freeUntilPos(NUM_REGS, MAX_LOCATION);

  bool is_fp = current->operand->isFp();

  for (auto& interval : active) {
    if (interval->operand->isFp() != is_fp) {
      continue;
    }

    freeUntilPos[interval->allocated_loc.loc] = START_LOCATION;
  }

  for (auto& interval : inactive) {
    if (interval->operand->isFp() != is_fp) {
      continue;
    }

    auto intersect = interval->intersectWith(*current);
    if (intersect != INVALID_LOCATION) {
      freeUntilPos[interval->allocated_loc.loc] =
          std::min(freeUntilPos[interval->allocated_loc.loc], intersect);
    }
  }

  markDisallowedRegisters(freeUntilPos);

  PhyLocation reg;
  LIRLocation regFreeUntil = START_LOCATION;

  // for preallocated intervals, try to honor the preallocated register.
  // the preallocated register is a soft constraint to the register
  // allocator. It will be satisfied with the best effort.
  if (current->isRegisterAllocated()) {
    PhyLocation allocated = current->allocated_loc;
    JIT_CHECK(
        is_fp == allocated.is_fp_register(),
        "Operand is allocated to register {} of incorrect type",
        allocated);
    if (freeUntilPos[allocated.loc] != START_LOCATION) {
      reg = allocated;
      regFreeUntil = freeUntilPos[allocated.loc];
    }
  }

  // if not preallocated interval or cannot honor the preallocated register
  if (regFreeUntil == START_LOCATION) {
    auto start = std::next(freeUntilPos.begin(), is_fp ? VECD_REG_BASE : 0);
    auto end = std::prev(freeUntilPos.end(), is_fp ? 0 : NUM_VECD_REGS);

    auto max_iter = std::max_element(start, end);
    if (*max_iter == START_LOCATION) {
      return false;
    }
    regFreeUntil = *max_iter;
    size_t bit_size = current->operand->sizeInBits();
    reg = PhyLocation(std::distance(freeUntilPos.begin(), max_iter), bit_size);
  }

  TRACE("Allocating free location {} to interval {}", reg, *current);
  current->allocateTo(reg);
  if (current->endLocation() > regFreeUntil) {
    splitAndSave(current, regFreeUntil, unhandled);
  }

  return true;
}

void LinearScanAllocator::allocateBlockedReg(
    LiveInterval* current,
    UnorderedSet<LiveInterval*>& active,
    UnorderedSet<LiveInterval*>& inactive,
    UnhandledQueue& unhandled) {
  std::vector<LIRLocation> nextUsePos(NUM_REGS, MAX_LOCATION);

  UnorderedMap<PhyLocation, LiveInterval*> reg_active_interval;
  UnorderedMap<PhyLocation, std::vector<LiveInterval*>> reg_inactive_intervals;

  bool is_fp = current->operand->isFp();

  auto current_start = current->startLocation();
  for (auto& interval : active) {
    if (interval->operand->isFp() != is_fp) {
      continue;
    }
    auto allocated_loc = interval->allocated_loc.loc;
    nextUsePos[allocated_loc] =
        getUseAtOrAfter(interval->operand, current_start);
    reg_active_interval.emplace(allocated_loc, interval);
  }
  for (auto& interval : inactive) {
    if (interval->operand->isFp() != is_fp) {
      continue;
    }
    auto intersect = interval->intersectWith(*current);
    auto allocated_loc = interval->allocated_loc.loc;
    if (intersect != INVALID_LOCATION) {
      nextUsePos[allocated_loc] = std::min(
          nextUsePos[allocated_loc],
          getUseAtOrAfter(interval->operand, current_start));
    }

    reg_inactive_intervals[allocated_loc].push_back(interval);
  }

  markDisallowedRegisters(nextUsePos);

  auto start = std::next(nextUsePos.begin(), is_fp ? VECD_REG_BASE : 0);
  auto end = std::prev(nextUsePos.end(), is_fp ? 0 : VECD_REG_BASE);

  auto reg_iter = std::max_element(start, end);
  PhyLocation reg(
      std::distance(nextUsePos.begin(), reg_iter),
      current->operand->sizeInBits());
  auto& reg_use = *reg_iter;

  auto first_current_use = getUseAtOrAfter(current->operand, current_start);
  if (first_current_use >= reg_use) {
    auto stack_slot = getStackSlot(current->operand);
    TRACE(
        "Allocating blocked location {} to interval {}", stack_slot, *current);
    current->allocateTo(stack_slot);

    // First_current_use can be MAX_LOCATION when the operand is in a loop and
    // there are no more uses after current_start.
    if (first_current_use < current->endLocation()) {
      splitAndSave(current, first_current_use, unhandled);
    }
  } else {
    TRACE("Allocating blocked location {} to interval {}", reg, *current);
    current->allocateTo(reg);

    auto act_iter = reg_active_interval.find(reg);
    JIT_CHECK(
        act_iter != reg_active_interval.end(),
        "Must have one active interval allocated to reg. Otherwise, this "
        "function wouldn't have been called.");
    auto act_interval = act_iter->second;

    if (current_start == act_interval->startLocation()) {
      active.erase(act_interval);
      unhandled.push(act_interval);
    } else {
      splitAndSave(act_interval, current_start, unhandled);
    }

    auto inact_iter = reg_inactive_intervals.find(reg);
    if (inact_iter != reg_inactive_intervals.end()) {
      for (auto& inact_interval : inact_iter->second) {
        // do not split fixed intervals here. if current and the fixed interval
        // overlap, it will be handled later.
        if (!inact_interval->fixed) {
          // since by definition current_start is in the lifetime hole of
          // inactive intervals, splitting at current_start is effectively
          // splitting at the end of the lifetime hole.
          splitAndSave(inact_interval, current_start, unhandled);
        } else {
          // check if current intersects with a fixed interval
          auto intersect = current->intersectWith(*inact_interval);
          if (intersect != INVALID_LOCATION) {
            splitAndSave(current, intersect, unhandled);
          }
        }
      }
    }
  }
}

LIRLocation LinearScanAllocator::getUseAtOrAfter(
    const Operand* operand,
    LIRLocation loc) const {
  auto vreg_use_iter = vreg_phy_uses_.find(operand);
  if (vreg_use_iter == vreg_phy_uses_.end()) {
    return MAX_LOCATION;
  }

  auto& vu = vreg_use_iter->second;
  auto iter = vu.lower_bound(loc);
  if (iter == vu.end()) {
    return MAX_LOCATION;
  }

  return *iter;
}

void LinearScanAllocator::splitAndSave(
    LiveInterval* interval,
    LIRLocation loc,
    UnhandledQueue& queue) {
  JIT_CHECK(
      interval->startLocation() < loc,
      "Invalid split point {} for interval {}",
      loc,
      *interval);
  auto new_interval = interval->splitAt(loc);
  JIT_CHECK(
      new_interval != nullptr,
      "Split point {} is not inside interval {}",
      loc,
      *interval);
  JIT_CHECK(
      new_interval->startLocation() < new_interval->endLocation(),
      "Invalid interval {}",
      *new_interval);

  TRACE("Split new interval {}", *new_interval);
  queue.push(new_interval.get());
  allocated_.emplace_back(std::move(new_interval));
}

PhyLocation LinearScanAllocator::getStackSlot(const Operand* operand) {
  auto iter = operand_to_slot_.find(operand);
  if (iter != operand_to_slot_.end()) {
    return iter->second;
  }

  PhyLocation slot = newStackSlot(operand);
  operand_to_slot_.emplace(operand, slot);
  return slot;
}

PhyLocation LinearScanAllocator::newStackSlot(const Operand* operand) {
  PhyLocation slot;
  if (free_stack_slots_.empty()) {
    // Intentionally align all new stack slots to 8-bytes, regardless of the
    // operand's size.  Uses more stack space but avoids alignment issues.
    max_stack_slot_ -= kPointerSize;
    slot = PhyLocation{max_stack_slot_, operand->sizeInBits()};
    TRACE("Allocating new stack slot {} for operand {}", slot, *operand);
  } else {
    slot = free_stack_slots_.back();
    // Update slot to the correct size.
    slot.bitSize = operand->sizeInBits();
    free_stack_slots_.pop_back();
    TRACE("Reusing stack slot {} for operand {}", slot, *operand);
  }
  JIT_CHECK(
      slot.is_memory(),
      "Incorrectly allocated {} for stack-allocated operand {}",
      slot,
      *operand);
  return slot;
}

void LinearScanAllocator::freeStackSlot(const Operand* operand) {
  auto iter = operand_to_slot_.find(operand);
  JIT_CHECK(
      iter != operand_to_slot_.end(),
      "Operand {} doesn't seem to have been allocated a stack slot",
      *operand);

  PhyLocation slot = iter->second;
  JIT_CHECK(
      slot.is_memory(),
      "Have mapped a stack-allocated operand {} to register {}",
      *operand,
      slot);

  operand_to_slot_.erase(operand);
  free_stack_slots_.push_back(slot);
}

void LinearScanAllocator::rewriteLIR() {
  UnorderedMap<const Operand*, const LiveInterval*> mapping;

  auto allocated_iter = allocated_.begin();

  UnorderedSet<const lir::LinkedOperand*> last_use_vregs;
  for (auto& use_pair : vreg_last_use_) {
    for (auto& pair : use_pair.second) {
      last_use_vregs.emplace(pair.first);
    }
  }

  // Update mappings for before the entry block.
  while (allocated_iter != allocated_.end() &&
         (*allocated_iter)->startLocation() <= START_LOCATION) {
    auto& interval = *allocated_iter;
    auto [_, inserted] = mapping.emplace(interval->operand, interval.get());
    JIT_CHECK(
        inserted,
        "Created duplicate mapping for operand {} in the entry block",
        *interval->operand);
    ++allocated_iter;
  }

  int instr_id = -1;
  for (BasicBlock* bb : func_->basicblocks()) {
    ++instr_id;
    TRACE("{} - Start basic block {}", instr_id, bb->id());

    // Remove mappings that end at the last basic block.
    // Inter-basic block resolution will be done later separately.
    for (auto map_iter = mapping.begin(); map_iter != mapping.end();) {
      auto [operand, interval] = *map_iter;
      JIT_CHECK(
          operand == interval->operand,
          "Mapping is not consistent: {} -> {}",
          *operand,
          *interval);

      if (interval->endLocation() <= instr_id) {
        TRACE("Removing interval {} for operand {}", *interval, *operand);
        map_iter = mapping.erase(map_iter);
      } else {
        ++map_iter;
      }
    }

    // handle the basic block id before instructions start
    while (allocated_iter != allocated_.end() &&
           (*allocated_iter)->startLocation() <= instr_id) {
      auto& interval = *allocated_iter;
      rewriteLIRUpdateMapping(mapping, interval.get(), nullptr);
      ++allocated_iter;
    }

    auto& instrs = bb->instructions();
    bool process_input = false;
    for (auto instr_iter = instrs.begin(); instr_iter != instrs.end();) {
      ++instr_id;
      process_input = !process_input;

      auto instr = instr_iter->get();
      TRACE("{} - {} - {}", instr_id, process_input ? "in" : "out", *instr);

      auto copies = std::make_unique<CopyGraphWithOperand>();
      // check for new allocated intervals and update register mappings
      while (allocated_iter != allocated_.end() &&
             (*allocated_iter)->startLocation() <= instr_id) {
        auto& interval = *allocated_iter;
        rewriteLIRUpdateMapping(mapping, interval.get(), copies.get());
        ++allocated_iter;
      }

      rewriteLIREmitCopies(bb, instr_iter, std::move(copies));

      if (process_input) {
        // phi node inputs have to be handled by its predecessor
        if (!instr->isPhi()) {
          rewriteInstrInputs(instr, mapping, &last_use_vregs);

          if (instr->output()->isInd()) {
            rewriteInstrOutput(instr, mapping, &last_use_vregs);
          }
          if (instr->isYieldInitial()) {
            computeInitialYieldSpillSize(mapping);
          }
        }
      } else {
        rewriteInstrOutput(instr, mapping, &last_use_vregs);

        if (instr->isNop()) {
          instr_iter = instrs.erase(instr_iter);
          continue;
        }

        TRACE("After rewrite: {}", *instr);
        ++instr_iter;
      }
    }

    // handle successors' phi nodes
    for (BasicBlock* succ : bb->successors()) {
      succ->foreachPhiInstr([&](Instruction* phi) {
        auto index = phi->getOperandIndexByPredecessor(bb);
        JIT_CHECK(
            index != -1,
            "Can't find predecessor block {} in phi instruction: {}",
            bb->id(),
            *phi);
        rewriteInstrOneInput(phi, index, mapping, nullptr /* last_use_vregs */);
      });
    }

    // record vreg-to-physical-location mapping at the end of each basic block,
    // which is needed for resolve edges.
    bb_vreg_end_mapping_.emplace(bb, mapping);
  }
}

void LinearScanAllocator::rewriteInstrOutput(
    Instruction* instr,
    const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
    const UnorderedSet<const LinkedOperand*>* last_use_vregs) {
  auto output = instr->output();
  if (output->isInd()) {
    rewriteInstrOneIndirectOperand(
        output->getMemoryIndirect(), mapping, last_use_vregs);
    return;
  }

  if (!output->isVreg()) {
    return;
  }

  auto interval = map_get(mapping, output, nullptr);
  if (interval != nullptr) {
    output->setPhyRegOrStackSlot(interval->allocated_loc);
    return;
  }

  // if we cannot find an allocated interval for an output, it means that
  // the output is not used in the program, and therefore the instruction
  // can be removed.
  // Avoid removing call instructions that may have side effects.
  // TODO: Fix HIR generator to avoid generating unused output/variables.
  // Need a separate pass in HIR to handle the dead code more gracefully.
  if (instr->opcode() == Instruction::kCall ||
      instr->opcode() == Instruction::kVarArgCall ||
      instr->opcode() == Instruction::kVectorCall) {
    output->setNone();
  } else {
    instr->setOpcode(Instruction::kNop);
  }
}

void LinearScanAllocator::rewriteInstrInputs(
    Instruction* instr,
    const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
    const UnorderedSet<const LinkedOperand*>* last_use_vregs) {
  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    rewriteInstrOneInput(instr, i, mapping, last_use_vregs);
  }
}

void LinearScanAllocator::rewriteInstrOneInput(
    Instruction* instr,
    size_t i,
    const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
    const UnorderedSet<const LinkedOperand*>* last_use_vregs) {
  auto input = instr->getInput(i);

  if (input->isInd()) {
    rewriteInstrOneIndirectOperand(
        input->getMemoryIndirect(), mapping, last_use_vregs);
    return;
  }

  if (!shouldReplaceOperand(*input) || input->isNone()) {
    return;
  }

  auto iter = mapping.find(input->getDefine());
  if (iter == mapping.end()) {
    JIT_CHECK(
        !input->isVreg(),
        "Can't find allocation for operand {}, for instruction {}",
        *input,
        *instr);
    return;
  }

  auto phyreg = iter->second->allocated_loc;
  auto new_input = std::make_unique<Operand>();
  new_input->setDataType(input->dataType());
  new_input->setPhyRegOrStackSlot(phyreg);

  if (last_use_vregs != nullptr &&
      last_use_vregs->count(static_cast<LinkedOperand*>(input))) {
    new_input->setLastUse();
  }

  instr->setInput(i, std::move(new_input));
}

void LinearScanAllocator::rewriteInstrOneIndirectOperand(
    MemoryIndirect* indirect,
    const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
    const UnorderedSet<const LinkedOperand*>* last_use_vregs) {
  auto base = indirect->getBaseRegOperand();
  PhyLocation base_phy_reg = shouldReplaceOperand(*base)
      ? map_get(mapping, base->getDefine())->allocated_loc
      : PhyLocation(base->getPhyRegister());

  bool base_last_use = last_use_vregs != nullptr && base->isLinked() &&
      last_use_vregs->count(static_cast<LinkedOperand*>(base));

  auto index = indirect->getIndexRegOperand();
  PhyLocation index_phy_reg = PhyLocation::REG_INVALID;
  bool index_last_use = false;
  if (index != nullptr) {
    index_phy_reg = shouldReplaceOperand(*index)
        ? map_get(mapping, index->getDefine())->allocated_loc
        : PhyLocation(index->getPhyRegister());

    index_last_use = last_use_vregs != nullptr && base->isLinked() &&
        last_use_vregs->count(static_cast<LinkedOperand*>(base));
  }
  indirect->setMemoryIndirect(
      base_phy_reg,
      index_phy_reg,
      indirect->getMultipiler(),
      indirect->getOffset());

  if (base_last_use) {
    indirect->getBaseRegOperand()->setLastUse();
  }

  if (index_last_use) {
    indirect->getIndexRegOperand()->setLastUse();
  }
}

void LinearScanAllocator::rewriteLIRUpdateMapping(
    UnorderedMap<const lir::Operand*, const LiveInterval*>& mapping,
    LiveInterval* interval,
    CopyGraphWithOperand* copies) {
  auto operand = interval->operand;
  auto [mapping_iter, inserted] = mapping.emplace(operand, interval);
  if (inserted) {
    TRACE("Adding interval {} for operand {}", *interval, *operand);
    return;
  }

  if (copies != nullptr) {
    auto from = mapping_iter->second->allocated_loc;
    auto to = interval->allocated_loc;
    if (from != to) {
      auto data_type = operand->dataType();
      TRACE("Adding copy {} -> {} with data type {}", from, to, data_type);
      copies->addEdge(from.loc, to.loc, data_type);
    }
  }

  TRACE("Updating interval {} for operand {}", *interval, *operand);
  mapping_iter->second = interval;
}

void LinearScanAllocator::resolveEdges() {
  // collect intervals that are live at beginning of a basic block
  UnorderedMap<BasicBlock*, std::vector<LiveInterval*>> bb_interval_map;
  auto& blocks = func_->basicblocks();

  for (auto& interval : allocated_) {
    auto start = interval->startLocation();
    auto end = interval->endLocation();

    // find the first basic block starting after the interval start
    auto iter = std::lower_bound(
        blocks.begin(),
        blocks.end(),
        start,
        [this](const auto& block, const auto start) -> bool {
          BasicBlock* bb = block;
          auto block_start = map_get(regalloc_blocks_, bb).block_start_index;
          return block_start < start;
        });

    for (; iter != blocks.end(); ++iter) {
      BasicBlock* block = *iter;
      auto block_start = map_get(regalloc_blocks_, block).block_start_index;
      // if the block starts after the interval, no need to check further.
      if (block_start >= end) {
        break;
      }

      // still need to call covers() due to liveness holes
      if (interval->covers(block_start)) {
        bb_interval_map[block].push_back(interval.get());
      }
    }
  }

  for (size_t block_index = 0; block_index < blocks.size(); block_index++) {
    auto basic_block = blocks.at(block_index);
    auto& successors = basic_block->successors();
    if (successors.empty()) {
      continue;
    }

    auto next_block_index = block_index + 1;
    auto next_basic_block = next_block_index == blocks.size()
        ? nullptr
        : blocks.at(next_block_index);

    auto& instrs = basic_block->instructions();
    bool empty = instrs.empty();
    auto last_instr_iter = empty ? instrs.end() : std::prev(instrs.end());
    auto last_instr = empty ? nullptr : last_instr_iter->get();

    auto last_instr_opcode =
        last_instr != nullptr ? last_instr->opcode() : Instruction::kNone;

    // for unconditional branch
    if (successors.size() == 1) {
      auto succ = successors.front();
      auto copies =
          resolveEdgesGenCopies(basic_block, succ, bb_interval_map[succ]);

      bool is_return = last_instr_opcode == Instruction::kReturn;
      if (is_return) {
        // check if the operand is the return register
        auto ret_opnd = last_instr->getInput(0);
        if (ret_opnd->isReg() || ret_opnd->isStack()) {
          auto reg = ret_opnd->getPhyRegOrStackSlot();

          auto target = ret_opnd->isFp() ? arch::reg_double_return_loc
                                         : arch::reg_general_return_loc;

          target.bitSize = reg.bitSize;

          if (reg != target) {
            copies->addEdge(reg.loc, target.loc, ret_opnd->dataType());
          }
        } else {
          // return <constant>, we need to shuffle the value into rax here
          auto instr = basic_block->allocateInstrBefore(
              std::prev(instrs.end()), Instruction::kMove);
          JIT_CHECK(!ret_opnd->isFp(), "only integer should be present");
          instr->allocateImmediateInput(ret_opnd->getConstant());
          instr->output()->setPhyRegister(arch::reg_general_return_loc);
          instr->output()->setDataType(ret_opnd->dataType());
        }
      }

      JIT_CHECK(
          last_instr_opcode != Instruction::kBranch,
          "Unconditional branch should not have been generated yet: {}",
          *last_instr);

      rewriteLIREmitCopies(
          basic_block, basic_block->instructions().end(), std::move(copies));

      if (is_return) {
        basic_block->removeInstr(last_instr_iter);
      }

      continue;
    }

    // for conditional branch
    // generate new trampoline basic blocks
    auto true_bb = successors.front();
    auto false_bb = successors.back();

    auto true_bb_copies =
        resolveEdgesGenCopies(basic_block, true_bb, bb_interval_map[true_bb]);
    auto false_bb_copies =
        resolveEdgesGenCopies(basic_block, false_bb, bb_interval_map[false_bb]);

    resolveEdgesInsertBasicBlocks(
        basic_block,
        next_basic_block,
        true_bb,
        false_bb,
        std::move(true_bb_copies),
        std::move(false_bb_copies));

    while (blocks.at(block_index) != next_basic_block) {
      block_index++;
    }
    block_index--;
  }
}

std::unique_ptr<LinearScanAllocator::CopyGraphWithOperand>
LinearScanAllocator::resolveEdgesGenCopies(
    const BasicBlock* basicblock,
    const BasicBlock* successor,
    std::vector<LiveInterval*>& intervals) {
  auto copies = std::make_unique<CopyGraphWithOperand>();
  auto& end_mapping = bb_vreg_end_mapping_[basicblock];
  auto& succ_regalloc_block = map_get(regalloc_blocks_, successor);

  for (auto interval : intervals) {
    // Check if the interval starts from the beginning of the successor
    // there are two cases where interval_starts_from_beginning can be true:
    //
    // 1. The interval associates with a vreg defined by a phi instruction.
    //
    // 2. The basic block has no phi instruction, and the vreg is defined by the
    //    first instruction.
    bool interval_starts_from_beginning =
        interval->startLocation() == succ_regalloc_block.block_start_index;

    // Phi will be set if case 1.
    const Instruction* phi = nullptr;
    if (interval_starts_from_beginning) {
      // In future optimizations, we can consider a way of looking up a phi by
      // vreg instead of linear scan.
      successor->foreachPhiInstr([&](const Instruction* instr) {
        if (instr->output()->getPhyRegOrStackSlot() ==
            interval->allocated_loc) {
          phi = instr;
        }
      });
    }

    PhyLocation from = 0;
    PhyLocation to = 0;
    DataType data_type;

    if (phi != nullptr) {
      auto operand = phi->getOperandByPredecessor(basicblock);
      from = operand->getPhyRegOrStackSlot();
      to = phi->output()->getPhyRegOrStackSlot();
      data_type = operand->dataType();
    } else if (interval_starts_from_beginning) {
      // If not Phi, we need to check the original first instruction.  Please
      // note here, we cannot get the original first instruction with
      // successor->getFirstInstr(), because the successor block may already
      // been rewritten, and the first instruction may not be the original
      // first instruction any more.
      auto succ_first_instr = succ_regalloc_block.block_first_instr;

      // Even though LIR is in SSA, when the successor is a loop head, the
      // first instruction could be a define of the same vreg.  In that case,
      // we don't need to generate move instructions.
      if (succ_first_instr->output() == interval->operand) {
        continue;
      }

      auto operand = interval->operand;
      auto from_interval = map_get(end_mapping, operand, nullptr);
      if (from_interval == nullptr) {
        continue;
      }
      from = from_interval->allocated_loc;
      to = interval->allocated_loc;
      data_type = from_interval->operand->dataType();
    } else {
      auto operand = interval->operand;
      auto from_interval = map_get(end_mapping, operand);
      from = from_interval->allocated_loc;
      to = interval->allocated_loc;
      data_type = from_interval->operand->dataType();
    }

    if (from != to) {
      TRACE(
          "Adding copy {} -> {} with data type {} for block edge {} -> {}",
          from,
          to,
          data_type,
          basicblock->id(),
          successor->id());
      copies->addEdge(from.loc, to.loc, data_type);
    }
  }

  return copies;
}

void LinearScanAllocator::rewriteLIREmitCopies(
    BasicBlock* block,
    instr_iter_t instr_iter,
    std::unique_ptr<CopyGraphWithOperand> copies) {
  for (auto op : copies->process()) {
    PhyLocation from = op.from;
    PhyLocation to = op.to;
    auto orig_opnd_size = op.type;

    // All push and pop operations have to be 8-bytes in size as that's the size
    // of all stack slots.
    switch (op.kind) {
      case CopyGraph::Op::Kind::kCopy: {
        if (to == CopyGraph::kTempLoc) {
          auto instr =
              block->allocateInstrBefore(instr_iter, Instruction::kPush);
          instr->allocatePhyRegOrStackInput(from)->setDataType(
              DataType::k64bit);
        } else if (from == CopyGraph::kTempLoc) {
          auto instr =
              block->allocateInstrBefore(instr_iter, Instruction::kPop);
          instr->output()->setPhyRegOrStackSlot(to);
          instr->output()->setDataType(DataType::k64bit);
        } else if (to.is_register() || from.is_register()) {
          auto instr =
              block->allocateInstrBefore(instr_iter, Instruction::kMove);
          instr->allocatePhyRegOrStackInput(from)->setDataType(orig_opnd_size);
          instr->output()->setPhyRegOrStackSlot(to);
          instr->output()->setDataType(orig_opnd_size);
        } else {
          auto push =
              block->allocateInstrBefore(instr_iter, Instruction::kPush);
          push->allocatePhyRegOrStackInput(from)->setDataType(DataType::k64bit);

          auto pop = block->allocateInstrBefore(instr_iter, Instruction::kPop);
          pop->output()->setPhyRegOrStackSlot(to);
          pop->output()->setDataType(DataType::k64bit);
        }
        break;
      }
      case CopyGraph::Op::Kind::kExchange: {
        JIT_CHECK(
            from.is_register() && to.is_register(),
            "Can only exchange registers, got {} and {}",
            from,
            to);
        auto instr =
            block->allocateInstrBefore(instr_iter, Instruction::kExchange);
        instr->allocatePhyRegisterInput(from)->setDataType(orig_opnd_size);
        instr->output()->setPhyRegOrStackSlot(to);
        instr->output()->setDataType(orig_opnd_size);
        break;
      }
    }
  }
}

// In the future, we should move basic block ordering to a separate pass.
void LinearScanAllocator::resolveEdgesInsertBasicBlocks(
    BasicBlock* basic_block,
    BasicBlock* next_basic_block,
    BasicBlock* true_bb,
    BasicBlock* false_bb,
    std::unique_ptr<CopyGraphWithOperand> true_copies,
    std::unique_ptr<CopyGraphWithOperand> false_copies) {
  // convert {true_need_copy, false_need_copy, next_true, next_false}
  // => {bb1_is_true_bb, gen_new_bb1, gen_new_bb2}
  static std::vector<std::tuple<bool, bool, bool>> truth_table{
      {0, 1, 0},
      {0, 0, 0},
      {1, 0, 0},
      {0, 0, 0}, // don't care - will never happen
      {0, 1, 0},
      {0, 1, 0},
      {0, 1, 0},
      {0, 0, 0}, // don't care
      {1, 1, 0},
      {1, 1, 0},
      {1, 1, 0},
      {0, 0, 0}, // don't care
      {1, 1, 1},
      {1, 1, 1},
      {0, 1, 1},
      {0, 0, 0} // don't care
  };

  bool next_true = next_basic_block == true_bb;
  bool next_false = next_basic_block == false_bb;
  bool true_need_copy = !true_copies->isEmpty();
  bool false_need_copy = !false_copies->isEmpty();

  size_t index = (true_need_copy << 3) | (false_need_copy << 2) |
      (next_true << 1) | next_false;
  const auto& result = truth_table[index];
  bool bb1_true = std::get<0>(result);
  BasicBlock* bb1 = bb1_true ? true_bb : false_bb;
  BasicBlock* bb2 = bb1_true ? false_bb : true_bb;
  bool gen_new_bb1 = std::get<1>(result);
  bool gen_new_bb2 = std::get<2>(result);

  BasicBlock* new_bb1 = nullptr;
  BasicBlock* new_bb2 = nullptr;

  if (gen_new_bb2) {
    new_bb2 = basic_block->insertBasicBlockBetween(bb2);
  }

  if (gen_new_bb1) {
    new_bb1 = basic_block->insertBasicBlockBetween(bb1);
  }

  // emit copies if necessary
  auto emit_copies = [&](BasicBlock* new_bb, const BasicBlock* bb) {
    if (!new_bb) {
      return;
    }

    rewriteLIREmitCopies(
        new_bb,
        new_bb->instructions().end(),
        std::move(bb == true_bb ? true_copies : false_copies));
  };

  emit_copies(new_bb1, bb1);
  emit_copies(new_bb2, bb2);
}

std::ostream& operator<<(std::ostream& out, const LiveRange& rhs) {
  out << "[" << rhs.start << ", " << rhs.end << ")";
  return out;
}

std::ostream& operator<<(std::ostream& out, const LiveInterval& rhs) {
  auto loc = rhs.allocated_loc;
  if (loc != PhyLocation::REG_INVALID) {
    out << "->";
    if (loc.is_register()) {
      out << "R" << static_cast<int>(loc.loc);
    } else {
      out << "[RBP - " << -loc.loc << "]";
    }
    out << ": ";
  }

  auto sep = "";
  for (auto& range : rhs.ranges) {
    out << sep << range;
    sep = ", ";
  }
  return out;
}

} // namespace jit::lir
