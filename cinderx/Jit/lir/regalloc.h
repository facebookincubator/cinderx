// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/copy_graph.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/lir/block.h"

#include <memory>
#include <ostream>

namespace jit::lir {

// This header file contains classes implementing linear scan register
// allocation. The algorithm employed is based on papers "Linear Scan Register
// Allocation on SSA Form" and "Optimized Interval Splitting in a Linear Scan
// Register Allocator" by C. Wimmer, et al. A location in LIR is represented by
// a basic block index and an instruction index pair. A range in LIR is
// represented by a two locations - start and end. All the ranges are
// half-opened, with start included and end excluded. A interval in LIR is
// composed of a set of ranges.

struct RegallocBlockState {
  const BasicBlock* bb;
  int block_start_index;
  // the first instruction of the basic block before rewrite.
  Instruction* block_first_instr;
  UnorderedSet<const Operand*> livein;

  RegallocBlockState(const BasicBlock* b, int index, Instruction* instr);
};

// Location index in LIR.
using LIRLocation = int;

constexpr LIRLocation START_LOCATION = 0;
constexpr LIRLocation INVALID_LOCATION = -1;
constexpr LIRLocation MAX_LOCATION = std::numeric_limits<LIRLocation>::max();

struct LiveRange {
  LiveRange(LIRLocation s, LIRLocation e);

  LIRLocation start;
  LIRLocation end;

  bool isInRange(const LIRLocation& loc) const;

  bool intersectsWith(const LiveRange& lr) const;
};

struct LiveInterval {
  explicit LiveInterval(const Operand* operand);

  const Operand* operand;

  std::vector<LiveRange> ranges;
  PhyLocation allocated_loc;
  bool fixed{false}; // whether the allocated_loc is fixed.

  void addRange(LiveRange range);
  void setFrom(LIRLocation loc);

  LIRLocation startLocation() const;
  LIRLocation endLocation() const;

  bool covers(LIRLocation loc) const;
  bool isEmpty() const;

  // the two functions below return the first intersect point with a
  // LiveRange or LiveInterval. If they are disjioint, return
  // INVALID_LOCATION.
  LIRLocation intersectWith(const LiveRange& range) const;
  LIRLocation intersectWith(const LiveInterval& interval) const;

  // split the current interval at location loc. After splitting, the
  // current object takes the first part of the original interval, and
  // the function returns a LiveInterval object pointer pointing to the second
  // part of the original interval. The new LiveInterval (second part)
  // starts either from loc (if loc falls into a LiveRange of the original
  // LiveInterval), or from the next LiveRange after loc (if loc falls outside
  // any LiveRange of the original LiveInterval).
  // If the current interval cannot be split at location loc, return nullptr.
  std::unique_ptr<LiveInterval> splitAt(LIRLocation loc);

  void allocateTo(PhyLocation loc);

  bool isAllocated() const;
  bool isRegisterAllocated() const;
};

// The linear scan allocator.
// The register allocator works in four steps:
//   1. reorder the basic blocks in RPO order,
//   2. calculate liveness intervals and use locations,
//   3. linear scan and allocate registers,
//   4. rewrite the original LIR.
class LinearScanAllocator {
 public:
  using IntervalMap = UnorderedMap<const Operand*, LiveInterval>;
  using IntervalList = std::vector<std::unique_ptr<LiveInterval>>;

  explicit LinearScanAllocator(Function* func, int reserved_stack_space = 0);

  void run();

  codegen::PhyRegisterSet getChangedRegs() const;

  // Return the number of bytes that should be allocated below the base
  // pointer, including zero or more shadow frames and any needed spill space.
  int getFrameSize() const;

  int initialYieldSpillSize() const;

  // returns true if the variables defined in the entry block is
  // used in the function.
  bool isPredefinedUsed(const Operand* operand) const;

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

  void spillRegistersForYield(int instr_id);
  void computeInitialYieldSpillSize(
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping);

  // Reserve all caller-saved registers for a function call.
  void reserveCallerSaveRegisters(int instr_id);

  // Reserve an arbitrary set of registers for an instruction, spilling them if
  // they are in use.
  void reserveRegisters(int instr_id, codegen::PhyRegisterSet phy_regs);

  void linearScan();
  bool tryAllocateFreeReg(
      LiveInterval* current,
      UnorderedSet<LiveInterval*>& active,
      UnorderedSet<LiveInterval*>& inactive,
      UnhandledQueue& unhandled);
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
      const UnorderedSet<const LinkedOperand*>* last_use_vregs);

  void rewriteInstrInputs(
      Instruction* instr,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const LinkedOperand*>* last_use_vregs);

  void rewriteInstrOneInput(
      Instruction* instr,
      size_t i,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const LinkedOperand*>* last_use_vregs);

  void rewriteInstrOneIndirectOperand(
      MemoryIndirect* indirect,
      const UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      const UnorderedSet<const LinkedOperand*>* last_use_vregs);

  // update virtual register to physical register mapping.
  // if the mapping is changed for a virtual register and copies is not nullptr,
  // insert a copy to copies for CopyGraph to generate a MOV instruction.
  void rewriteLIRUpdateMapping(
      UnorderedMap<const Operand*, const LiveInterval*>& mapping,
      LiveInterval* interval,
      CopyGraphWithOperand* copies);

  // emit copies before instr_iter
  void rewriteLIREmitCopies(
      BasicBlock* block,
      instr_iter_t instr_iter,
      std::unique_ptr<CopyGraphWithOperand> copies);

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

  UnorderedMap<const Operand*, OrderedSet<LIRLocation>> vreg_phy_uses_;

  UnorderedMap<const BasicBlock*, RegallocBlockState> regalloc_blocks_;

  // collect the last uses for all the vregs
  // key: def operand
  // value: a map with key: the use operand
  //                   value: use location
  UnorderedMap<const Operand*, UnorderedMap<const LinkedOperand*, LIRLocation>>
      vreg_last_use_;

  // the global last use of an operand (vreg)
  UnorderedMap<const Operand*, LIRLocation> vreg_global_last_use_;

  int initial_max_stack_slot_;
  int max_stack_slot_;
  std::vector<PhyLocation> free_stack_slots_;

  codegen::PhyRegisterSet changed_regs_;
  int initial_yield_spill_size_{-1};

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
};

std::ostream& operator<<(std::ostream& out, const LiveRange& rhs);
std::ostream& operator<<(std::ostream& out, const LiveInterval& rhs);

} // namespace jit::lir

template <>
struct fmt::formatter<jit::lir::LiveRange> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::lir::LiveInterval> : fmt::ostream_formatter {};
