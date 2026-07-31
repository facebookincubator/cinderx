// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/containers.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/copy_graph.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/regalloc.h"

#include <memory>
#include <ostream>

namespace cinderx::jit::lir {

// This header file contains classes implementing linear scan register
// allocation. The algorithm employed is based on papers "Linear Scan Register
// Allocation on SSA Form" and "Optimized Interval Splitting in a Linear Scan
// Register Allocator" by C. Wimmer, et al.

// A flattened position in the program, monotonically increasing.  Each
// instruction owns two consecutive locations: the first for reading its inputs,
// the second for defining its output.
using LIRLocation = int;

// The location before the first instruction of the function.
constexpr LIRLocation START_LOCATION = 0;
// Sentinel for "no such location", e.g. a nonexistent intersection point.
constexpr LIRLocation INVALID_LOCATION = -1;
// Sentinel ordered after every real location, e.g. "no use after this point".
constexpr LIRLocation MAX_LOCATION = std::numeric_limits<LIRLocation>::max();

// A half-open range of locations [start, end) over which a value is live.
struct LiveRange {
  LiveRange(LIRLocation s, LIRLocation e);

  LIRLocation start;
  LIRLocation end;

  bool isInRange(const LIRLocation& loc) const;

  bool intersectsWith(const LiveRange& lr) const;
};

// The lifetime information of an LIR value (operand).  It is composed of a set
// of location ranges in the program where the value is live.
class LiveInterval {
 public:
  explicit LiveInterval(const Operand* operand);

  // Add a new live range for the operand to the interval.
  void addRange(LiveRange range);

  // Shorten the interval's first range so it begins at a value's def point,
  // dropping the range entirely if the def is at or past its end.
  void setFrom(LIRLocation loc);

  // Append a range for a fixed physical-register reservation.  Unlike addRange,
  // ranges are appended out of order (cheap) and must be finalized with
  // sortAndMergeRanges() once all reservations have been added.
  void appendFixedRange(LIRLocation start, LIRLocation end);

  // Sort ranges by start and merge overlapping or adjacent ones.  Used to
  // finalize fixed intervals built up with appendFixedRange().
  void sortAndMergeRanges();

  LIRLocation startLocation() const;
  LIRLocation endLocation() const;

  // Whether loc falls within one of the interval's live ranges.
  bool covers(LIRLocation loc) const;
  bool isEmpty() const;

  // Get the first intersection point with a LiveRange or a LiveInterval. If
  // they are disjoint, return INVALID_LOCATION.
  LIRLocation intersectWith(const LiveRange& range) const;
  LIRLocation intersectWith(const LiveInterval& interval) const;

  // Split the current interval at a given location.  After splitting, the
  // current object takes the first part of the original interval, and the
  // function returns a new LiveInterval object pointer pointing to the second
  // part of the original interval.
  //
  // The new LiveInterval starts either from `loc` (if `loc` falls into a
  // LiveRange of the original LiveInterval), or from the next LiveRange after
  // `loc` (if `loc` falls outside any LiveRange of the original LiveInterval).
  // If the current interval cannot be split at `loc`, return nullptr.
  std::unique_ptr<LiveInterval> splitAt(LIRLocation loc);

  // Assign the interval to a physical register or stack slot.
  void allocateTo(PhyLocation loc);

  // Like allocateTo, but mark the assignment as a hard constraint.  A fixed
  // interval is a physical-register reservation that must not be split or
  // spilled.
  void fixTo(PhyLocation loc);

  bool isAllocated() const;
  bool isRegisterAllocated() const;
  bool isFixed() const;

  const Operand* operand() const;
  const std::vector<LiveRange>& ranges() const;
  PhyLocation allocatedLoc() const;

 private:
  // The value whose liveness is being tracked.
  const Operand* operand_;

  // List of location ranges where the value is live.
  std::vector<LiveRange> ranges_;

  // Location where the interval's value is allocated to.
  PhyLocation allocated_loc_;

  // Index of the range examined by the last covers() call, used as a hint for
  // covers().
  mutable uint32_t range_hint_{0};

  // Whether the allocated location is fixed and cannot be spilled or split.
  bool fixed_{false};
};

// Per-basic-block state gathered during live interval calculation and reused
// when rewriting LIR and resolving cross-block edges.
struct RegallocBlockState {
  const BasicBlock* bb;
  // The block's first location, just before its first instruction.
  LIRLocation start;
  // The first instruction of the basic block before rewrite.  Rewriting mutates
  // the block, so this is captured up front for edge resolution.
  Instruction* first_instr;
  // Operands live on entry to the block.
  UnorderedSet<const Operand*> livein;

  RegallocBlockState(
      const BasicBlock* bb,
      LIRLocation block_start,
      Instruction* instr);
};

// The linear scan allocator. It works in four steps:
//   1. Reorder the basic blocks in RPO order.
//   2. Calculate liveness intervals and use locations.
//   3. Linear scan and allocate registers.
//   4. Rewrite the original LIR.
class LinearScanAllocator : public RegisterAllocator {
 public:
  using IntervalMap = UnorderedMap<const Operand*, LiveInterval>;
  using IntervalList = std::vector<std::unique_ptr<LiveInterval>>;

  explicit LinearScanAllocator(Function* func, int reserved_stack_space = 0);

  void run() override;

  codegen::PhyRegisterSet getChangedRegs() const override;
  int getFrameSize() const override;

  // Get the mapping of virtual registers to liveness intervals.  Meant for
  // tests.
  const IntervalMap& intervalMap() const;

  // Get the list of liveness intervals.  Meant for tests.
  const IntervalList& intervalList() const;

 private:
  struct LiveIntervalPtrGreater {
    bool operator()(const LiveInterval* lhs, const LiveInterval* rhs) const {
      return rhs->startLocation() < lhs->startLocation();
    }
  };

  using UnhandledQueue = std::priority_queue<
      LiveInterval*,
      std::vector<LiveInterval*>,
      LiveIntervalPtrGreater>;

  using CopyGraphWithOperand = codegen::CopyGraphWithType<const DataType>;

  // Get the interval for an operand.
  LiveInterval& getInterval(const Operand* operand);

  void calculateLiveIntervals();

  // Reserve registers for a function call, spilling all allocatable registers
  // when free-threaded stack scanning needs every live value in spill data.
  void reserveRegistersForCall(const Instruction& instr, LIRLocation instr_loc);

  // Reserve an arbitrary set of registers for an instruction, spilling them if
  // they are in use.
  void reserveRegisters(
      LIRLocation instr_loc,
      codegen::PhyRegisterSet phy_regs);

  // The main allocation loop: walk intervals in start order, maintaining the
  // active and inactive sets, and assign each interval a register or stack
  // slot.
  void linearScan();

  // Try to give `current` a register that is free for its whole lifetime,
  // splitting it if a register is only free for part of it.  Return false if no
  // register is available.
  bool tryAllocateFreeReg(
      LiveInterval* current,
      UnorderedSet<LiveInterval*>& active,
      UnorderedSet<LiveInterval*>& inactive,
      UnhandledQueue& unhandled);

  // Make room for `current` by spilling either `current` or an
  // already-allocated interval, whichever has the further-away next use.
  void allocateBlockedReg(
      LiveInterval* current,
      UnorderedSet<LiveInterval*>& active,
      UnorderedSet<LiveInterval*>& inactive,
      UnhandledQueue& unhandled);

  // Get the next use of a physical register for the vreg at or after a
  // location.
  LIRLocation getUseAtOrAfter(const Operand* vreg, LIRLocation loc) const;

  // Split at loc and save the new interval to unhandled and allocated_.
  void
  splitAndSave(LiveInterval* interval, LIRLocation loc, UnhandledQueue& queue);

  PhyLocation getStackSlot(const Operand* operand);
  PhyLocation newStackSlot(const Operand* operand);
  void freeStackSlot(const Operand* operand);

  void rewriteLIR();

  void rewriteInstrOutput(
      Instruction* instr,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const Operand*>* last_use_vregs);

  void rewriteInstrInputs(
      Instruction* instr,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const Operand*>* last_use_vregs);

  void rewriteInstrOneInput(
      Instruction* instr,
      size_t i,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const Operand*>* last_use_vregs);

  void rewriteInstrOneIndirectOperand(
      MemoryIndirect* indirect,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const Operand*>* last_use_vregs);

  // Update the virtual register to physical register mapping.  If the mapping
  // is changed for a virtual register and `copies` is not nullptr, insert a
  // copy to `copies` for CopyGraph to generate a MOV instruction.
  void rewriteLIRUpdateMapping(
      UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      LiveInterval* interval,
      CopyGraphWithOperand* copies);

  // Emit copies before `instr_iter`.
  void rewriteLIREmitCopies(
      BasicBlock* block,
      instr_iter_t instr_iter,
      CopyGraphWithOperand& copies);

  // Resolve allocations across block boundaries by emitting extra copies.
  void resolveEdges();

  std::unique_ptr<CopyGraphWithOperand> resolveEdgesGenCopies(
      const BasicBlock* basicblock,
      const BasicBlock* successor,
      std::vector<LiveInterval*>& intervals);

  /* this function allocates (up to two) basic blocks for conditional branch and
   * connects them as shown below:
   *
   *          +---------------------------+
   *          | jump_if_zero              |
   *          |                           v
   *  <basic_block> ----> <new_bb1>  <new_bb2>
   *                          |           |
   *                          |           +------> bb2
   *                          +------------------> bb1
   */
  void resolveEdgesInsertBasicBlocks(
      BasicBlock* basic_block,
      BasicBlock* next_basic_block,
      BasicBlock* true_bb,
      BasicBlock* false_bb,
      std::unique_ptr<CopyGraphWithOperand> true_copies,
      std::unique_ptr<CopyGraphWithOperand> false_copies);

  Function* func_;

  // Map of LIR values to their liveness intervals.  Used during live interval
  // calculation, but not during LIR rewriting.
  //
  // Meant for virtual registers but also contains intervals for physical
  // registers, for instructions that require specific registers.
  IntervalMap intervals_;

  // List of liveness intervals, sorted by start location.  These intervals hold
  // the allocated locations, unlike intervals_.  This can also contain multiple
  // intervals for the same operand, because of splitting.
  IntervalList allocated_;

  // For each operand, the sorted locations where it must occupy a physical
  // register (register uses and fixed reservations).  Drives next-use lookups
  // during spilling via getUseAtOrAfter().
  UnorderedMap<const Operand*, OrderedSet<LIRLocation>> vreg_phy_uses_;

  // Per-block state, keyed by basic block.
  UnorderedMap<const BasicBlock*, RegallocBlockState> regalloc_blocks_;

  // collect the last uses for all the vregs
  // key: def operand
  // value: a map with key: the use operand
  //                   value: use location
  UnorderedMap<const Operand*, UnorderedMap<const Operand*, LIRLocation>>
      vreg_last_use_;

  // The global last use of an operand (vreg).
  UnorderedMap<const Operand*, LIRLocation> vreg_global_last_use_;

  // Stack slots grow downward (toward more negative offsets).

  // The starting stack offset, below any reserved space.
  int initial_max_stack_slot_;
  // The lowest offset allocated so far.
  int max_stack_slot_;
  // Collection of freed slots available for reuse.
  std::vector<PhyLocation> free_stack_slots_;

  // Physical registers assigned during allocation.
  codegen::PhyRegisterSet changed_regs_;

  // record vreg-to-physical-location mapping at the end of each basic block,
  // which is needed for resolve edges.
  UnorderedMap<
      const BasicBlock*,
      UnorderedMap<const Operand*, const LiveInterval*>>
      bb_vreg_end_mapping_;

  // Map of operands to stack slots upon spilling.
  UnorderedMap<const Operand*, PhyLocation> operand_to_slot_;

  FRIEND_TEST(LinearScanAllocatorTest, RegAllocationNoSpill);
  FRIEND_TEST(LinearScanAllocatorTest, RegAllocation);
  FRIEND_TEST(
      LinearScanAllocatorTest,
      ArbitraryExecutionCallReservesAllRegistersInFreeThreadedBuild);
};

std::ostream& operator<<(std::ostream& out, const LiveRange& rhs);
std::ostream& operator<<(std::ostream& out, const LiveInterval& rhs);

} // namespace cinderx::jit::lir

template <>
struct fmt::formatter<cinderx::jit::lir::LiveRange> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<cinderx::jit::lir::LiveInterval>
    : fmt::ostream_formatter {};
