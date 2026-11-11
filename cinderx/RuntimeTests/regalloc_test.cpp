// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/linear_scan.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/spill_alloc.h"
#include "cinderx/RuntimeTests/lir_parser.h"

#include <fmt/ostream.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <vector>

using namespace cinderx::jit;

namespace cinderx::jit::lir {
class LinearScanAllocatorTest : public ::testing::Test {
 public:
  static BasicBlock* buildCriticalEdgePhiLIR(Function& function) {
    BasicBlock* entry = function.allocateBasicBlock();
    BasicBlock* fallthrough = function.allocateBasicBlock();
    BasicBlock* phi_block = function.allocateBasicBlock();
    BasicBlock* epilogue = function.allocateBasicBlock();
    entry->addSuccessor(fallthrough);
    IncomingEdge entry_edge = entry->addSuccessor(phi_block);
    IncomingEdge fallthrough_edge = fallthrough->addSuccessor(phi_block);
    phi_block->addSuccessor(epilogue);

    Instruction* entry_value =
        entry->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{10});
    Instruction* condition =
        entry->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{1});
    entry->allocateInstr(
        Opcode::kCondBranch,
        nullptr,
        VReg{condition},
        Lbl{fallthrough},
        Lbl{phi_block});
    Instruction* fallthrough_value =
        fallthrough->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{20});
    Instruction* phi =
        phi_block->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
    phi->addPhiInput(fallthrough_edge, fallthrough_value);
    phi->addPhiInput(entry_edge, entry_value);
    phi_block->allocateInstr(Opcode::kReturn, nullptr, VReg{phi});
    return phi_block;
  }

  static bool LiveIntervalPtrLess(
      const LiveInterval* lhs,
      const LiveInterval* rhs) {
    if (lhs->operand() == rhs->operand()) {
      return lhs->startLocation() < rhs->startLocation();
    }
    return lhs->operand() < rhs->operand();
  }

  UnorderedMap<const Operand*, int> buildOperandToIndexMap(
      const UnorderedMap<int, Instruction*>& map) {
    UnorderedMap<const Operand*, int> res;
    for (auto& m : map) {
      res.emplace(m.second->output(), m.first);
    }
    return res;
  }

  void runAllocator(Function* func) {
    LinearScanAllocator allocator{func};
    allocator.run();
  }

  codegen::PhyRegisterSet fixedRegisterIntervals(
      const std::vector<std::optional<LiveInterval>>& intervals) {
    codegen::PhyRegisterSet result;
    for (const auto& interval : intervals) {
      if (interval.has_value() && interval->isFixed() &&
          interval->isRegisterAllocated()) {
        result.set(interval->allocatedLoc());
      }
    }
    return result;
  }
};

TEST_F(LinearScanAllocatorTest, ExchangePreservesBothValueWidths) {
  struct ExchangeCase {
    int first_reg;
    DataType first_type;
    DataType second_type;
    bool is_fp;
  };
  const ExchangeCase cases[] = {
      {0, DataType::k32bit, DataType::kObject, false},
      {0, DataType::kObject, DataType::k32bit, false},
      {codegen::VECD_REG_BASE, DataType::kDouble, DataType::kDouble, true},
  };

  for (const ExchangeCase& test_case : cases) {
    SCOPED_TRACE(
        ::testing::Message()
        << test_case.first_type << " <-> " << test_case.second_type);
    Function func;
    BasicBlock* block = func.allocateBasicBlock();
    LinearScanAllocator allocator{&func};
    LinearScanAllocator::CopyGraphWithOperand copies;
    copies.addEdge(
        test_case.first_reg, test_case.first_reg + 1, test_case.first_type);
    copies.addEdge(
        test_case.first_reg + 1, test_case.first_reg, test_case.second_type);
    allocator.rewriteLIREmitCopies(block, block->instructions().end(), copies);

    ASSERT_EQ(block->getNumInstrs(), 1);
    Instruction* exchange = block->getFirstInstr();
    ASSERT_EQ(exchange->opcode(), Opcode::kExchange);
    size_t required_bits =
        std::max(bitSize(test_case.first_type), bitSize(test_case.second_type));
    EXPECT_GE(exchange->getInput(0)->sizeInBits(), required_bits);
    EXPECT_GE(exchange->output()->sizeInBits(), required_bits);
    EXPECT_EQ(exchange->getInput(0)->isFp(), test_case.is_fp);
    EXPECT_EQ(exchange->output()->isFp(), test_case.is_fp);
  }
}

TEST_F(LinearScanAllocatorTest, IntervalIntersectWithRange) {
  Operand operand;

  LiveInterval i1{&operand};
  i1.addRange(LiveRange{10, 30});
  i1.addRange(LiveRange{40, 60});

  EXPECT_EQ(i1.intersectWith(LiveRange{0, 10}), INVALID_LOCATION);
  EXPECT_EQ(i1.intersectWith(LiveRange{10, 11}), 10);
  EXPECT_EQ(i1.intersectWith(LiveRange{20, 50}), 20);
  EXPECT_EQ(i1.intersectWith(LiveRange{35, 45}), 40);
  EXPECT_EQ(i1.intersectWith(LiveRange{40, 50}), 40);
  EXPECT_EQ(i1.intersectWith(LiveRange{50, 70}), 50);
  EXPECT_EQ(i1.intersectWith(LiveRange{60, 65}), INVALID_LOCATION);
}

TEST_F(LinearScanAllocatorTest, IntervalIntersectWithInterval) {
  Operand operand1;
  Operand operand2;

  LiveInterval i1{&operand1};
  LiveInterval i2{&operand2};

  EXPECT_EQ(i1.intersectWith(i2), INVALID_LOCATION);
  EXPECT_EQ(i2.intersectWith(i1), INVALID_LOCATION);

  i1.addRange(LiveRange{20, 50});
  i2.addRange(LiveRange{10, 30});
  i2.addRange(LiveRange{40, 60});
  EXPECT_EQ(i1.intersectWith(i2), 20);
  EXPECT_EQ(i2.intersectWith(i1), 20);

  // Make sure the results are unchanged if i1 has more ranges than i2.
  i1.addRange(LiveRange{100, 200});
  i1.addRange(LiveRange{300, 400});
  EXPECT_EQ(i1.intersectWith(i2), 20);
  EXPECT_EQ(i2.intersectWith(i1), 20);
}

TEST_F(LinearScanAllocatorTest, RegAllocationNoSpill) {
  Function lir_func;
  BasicBlock* entry = lir_func.allocateBasicBlock();
  BasicBlock* branch = lir_func.allocateBasicBlock();
  BasicBlock* left = lir_func.allocateBasicBlock();
  BasicBlock* right = lir_func.allocateBasicBlock();
  BasicBlock* join = lir_func.allocateBasicBlock();
  BasicBlock* epilogue = lir_func.allocateBasicBlock();
  entry->addSuccessor(branch);
  branch->addSuccessor(left);
  branch->addSuccessor(right);
  IncomingEdge left_edge = left->addSuccessor(join);
  IncomingEdge right_edge = right->addSuccessor(join);
  join->addSuccessor(epilogue);

  Instruction* initial =
      entry->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{0});
  entry->allocateInstr(Opcode::kBranch, nullptr, Lbl{branch});
  Instruction* condition = branch->allocateInstr(
      Opcode::kAdd, nullptr, OutVReg{}, VReg{initial}, Imm{8});
  branch->allocateInstr(
      Opcode::kCondBranch, nullptr, VReg{condition}, Lbl{left}, Lbl{right});
  Instruction* left_value = left->allocateInstr(
      Opcode::kAdd, nullptr, OutVReg{}, VReg{initial}, Imm{8});
  left->allocateInstr(Opcode::kBranch, nullptr, Lbl{join});
  Instruction* right_value = right->allocateInstr(
      Opcode::kAdd, nullptr, OutVReg{}, VReg{initial}, Imm{16});
  right->allocateInstr(Opcode::kBranch, nullptr, Lbl{join});
  Instruction* phi = join->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  phi->addPhiInput(left_edge, left_value);
  phi->addPhiInput(right_edge, right_value);
  join->allocateInstr(Opcode::kReturn, nullptr, VReg{phi});

  struct ExpectedInterval {
    const char* name;
    const Operand* operand;
    std::initializer_list<LiveRange> ranges;
    std::initializer_list<LIRLocation> uses;
    int32_t register_index;
  };
  const ExpectedInterval expected_intervals[] = {
      {"initial", initial->output(), {{2, 12}, {15, 17}}, {2, 6, 11, 16}, 0},
      {"condition", condition->output(), {{7, 9}}, {7, 8}, 1},
      {"left_value", left_value->output(), {{12, 15}}, {12}, 0},
      {"right_value", right_value->output(), {{17, 20}}, {17}, 0},
      {"phi", phi->output(), {{20, 24}}, {}, 0},
  };

  LinearScanAllocator lsallocator(&lir_func);
  lsallocator.calculateLiveIntervals();
  ASSERT_EQ(lsallocator.vreg_intervals_.size(), std::size(expected_intervals));
  for (const ExpectedInterval& expected : expected_intervals) {
    SCOPED_TRACE(expected.name);
    const LiveInterval& interval = lsallocator.getInterval(expected.operand);
    const auto& ranges = interval.ranges();
    ASSERT_EQ(ranges.size(), expected.ranges.size());
    size_t range_index = 0;
    for (const LiveRange& range : expected.ranges) {
      EXPECT_EQ(ranges[range_index].start, range.start);
      EXPECT_EQ(ranges[range_index].end, range.end);
      ++range_index;
    }

    const auto& uses = lsallocator.vreg_phy_uses_.at(interval.vregId());
    ASSERT_EQ(uses.size(), expected.uses.size());
    size_t use_index = 0;
    for (LIRLocation use : expected.uses) {
      EXPECT_EQ(uses[use_index++], use);
    }
  }

  lsallocator.linearScan();

  EXPECT_EQ(lsallocator.getFrameSize(), 0);
  const auto& allocations = lsallocator.intervalList();
  ASSERT_EQ(allocations.size(), std::size(expected_intervals));
  for (const ExpectedInterval& expected : expected_intervals) {
    SCOPED_TRACE(expected.name);
    auto allocated = std::find_if(
        allocations.begin(), allocations.end(), [&](const auto& interval) {
          return interval->operand() == expected.operand;
        });
    ASSERT_NE(allocated, allocations.end());
    EXPECT_TRUE((*allocated)->isRegisterAllocated());
    EXPECT_EQ((*allocated)->allocatedLoc().loc, expected.register_index);
  }
}

TEST_F(LinearScanAllocatorTest, RegAllocation) {
  const char* lir_source = R"(Function:
BB %0 - succs: %5 %8
  %1 = Move 0(0x0)
  %2 = Add %1, 0(0x0)
  %3 = Add %1, 8(0x8)
  CondBranch %2, BB%5, BB%8
BB %5 - succs: %25
  %6 = Call 1024(0x400), %2, %3
  Branch BB%25
BB %8 - succs: %25
  %9 = Add %2, %3
  %10 = Add %9, 1
  %11 = Add %10, 1
  %12 = Add %11, 1
  %13 = Add %12, 1
  %14 = Add %13, 1
  %15 = Add %14, 1
  %16 = Add %15, 1
  %17 = Add %16, 1
  %18 = Call 1024(0x400), %3, %2, %9, %12, %13, %14, %15, %16, %17
  %19 = Add %2, %9
  %20 = Add %10, %11
  %21 = Add %20, %13
  %22 = Add %21, %15
  %23 = Call 1024(0x400), %19, %18, %22
  Branch BB%25
BB %25 - succs: %28
  %26 = Phi (BB%8, %23), (BB%5, %6)
  Return %26
BB %28

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);
  auto opnd_id_map = buildOperandToIndexMap(parser.getOutputInstrMap());

  LinearScanAllocator lsallocator(lir_func.get());
  lir_func->sortBasicBlocks();
  lsallocator.calculateLiveIntervals();
  lsallocator.linearScan();
  ASSERT_FALSE(lsallocator.intervalList().empty());

  ASSERT_GT(lsallocator.getFrameSize(), 0)
      << "Incorrect results - no registers have been spilled.";

  std::vector<LiveInterval*> intervals;
  UnorderedMap<int, std::vector<LiveInterval*>> loc_interval_map;
  UnorderedMap<const Operand*, std::vector<LiveInterval*>> vreg_location_map;
  for (auto& alloc : lsallocator.intervalList()) {
    if (!opnd_id_map.contains(alloc->operand())) {
      continue;
    }
    loc_interval_map[alloc->allocatedLoc().loc].push_back(alloc.get());
    vreg_location_map[alloc->operand()].push_back(alloc.get());
  }

  // check if the intervals allocated to the same location do not overlop
  for (auto& pair : loc_interval_map) {
    auto& loc_intervals = pair.second;
    for (size_t i = 0; i < loc_intervals.size() - 1; i++) {
      for (size_t j = i + 1; j < loc_intervals.size(); j++) {
        auto ival1 = loc_intervals[i];
        auto ival2 = loc_intervals[j];
        auto intersection = ival1->intersectWith(*ival2);

        ASSERT_EQ(intersection, -1) << fmt::format(
            "Location {} has conflicting intervals: {} intersects with {}",
            pair.first,
            *ival1,
            *ival2);
      }
    }
  }

  // check if the same virtual register does not allocate to multiple locations
  // at the same time
  for (auto& pair : vreg_location_map) {
    auto& vreg_intervals = pair.second;
    for (size_t i = 0; i < vreg_intervals.size() - 1; i++) {
      for (size_t j = i + 1; j < vreg_intervals.size(); j++) {
        auto ival1 = vreg_intervals[i];
        auto ival2 = vreg_intervals[j];
        auto intersection = ival1->intersectWith(*ival2);

        ASSERT_EQ(intersection, -1) << fmt::format(
            "Vreg {} has conflicting intervals: {} intersects with {}",
            opnd_id_map.at(pair.first),
            *ival1,
            *ival2);
      }
    }
  }
}

TEST_F(LinearScanAllocatorTest, RewriteSpilledMoveInputAsLoad) {
  auto function = std::make_unique<Function>();
  BasicBlock* block = function->allocateBasicBlock();
  Instruction* source =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{0});
  Instruction* move = block->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutPhyReg{codegen::arch::reg_general_return_loc},
      VReg{source});

  LinearScanAllocator allocator{function.get()};
  allocator.initializeVRegs();
  const RegAllocVRegId source_id = allocator.getVRegId(source->output());

  LiveInterval interval{source->output(), source_id};
  interval.allocateTo(PhyLocation{-kPointerSize});
  LinearScanAllocator::IntervalMapping mapping{{source_id, &interval}};

  allocator.rewriteInstrOneInput(move, 0, mapping, nullptr);

  EXPECT_TRUE(move->isLoad());
  EXPECT_TRUE(move->getInput(0)->isStack());
}

TEST_F(LinearScanAllocatorTest, FullRunHandlesCriticalEdgePhiSlots) {
  Function function;
  BasicBlock* phi_block = buildCriticalEdgePhiLIR(function);
  Instruction* phi = phi_block->getFirstInstr();

  LinearScanAllocator allocator{&function};
  allocator.run();

  ASSERT_EQ(phi->numPhiInputs(), 2);
  for (size_t slot = 0; slot < phi->numPhiInputs(); ++slot) {
    const Operand* value = phi->phiInput(slot);
    EXPECT_FALSE(value->isLinked());
    EXPECT_TRUE(value->isReg() || value->isStack());
    EXPECT_EQ(value->instr(), phi);
  }
}

TEST_F(LinearScanAllocatorTest, FullRunHandlesDuplicateEdgePhiSlots) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* phi_block = function.allocateBasicBlock();
  BasicBlock* epilogue = function.allocateBasicBlock();
  IncomingEdge first_edge = source->addSuccessor(phi_block);
  IncomingEdge second_edge = source->addSuccessor(phi_block);
  phi_block->addSuccessor(epilogue);

  Instruction* first_value =
      source->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{10});
  Instruction* second_value =
      source->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{20});
  source->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{first_value},
      Lbl{phi_block},
      Lbl{phi_block});
  Instruction* phi = phi_block->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  phi->addPhiInput(first_edge, first_value);
  phi->addPhiInput(second_edge, second_value);
  phi_block->allocateInstr(Opcode::kReturn, nullptr, VReg{phi});

  LinearScanAllocator allocator{&function};
  allocator.run();

  ASSERT_EQ(source->successors().size(), 2);
  EXPECT_NE(source->successors()[0], source->successors()[1]);
  size_t trampoline_count = 0;
  for (BasicBlock* successor : source->successors()) {
    if (successor == phi_block) {
      continue;
    }
    ++trampoline_count;
    EXPECT_EQ(successor->successors(), (std::vector<BasicBlock*>{phi_block}));
    EXPECT_GT(successor->getNumInstrs(), 0) << function;
  }
  EXPECT_GT(trampoline_count, 0);

  ASSERT_EQ(phi->numPhiInputs(), 2);
  for (size_t slot = 0; slot < phi->numPhiInputs(); ++slot) {
    const Operand* value = phi->phiInput(slot);
    EXPECT_FALSE(value->isLinked());
    EXPECT_TRUE(value->isReg() || value->isStack());
    EXPECT_EQ(value->instr(), phi);
  }
}

TEST_F(LinearScanAllocatorTest, FullRunHandlesLoopPhiSlots) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* loop = function.allocateBasicBlock();
  BasicBlock* exit = function.allocateBasicBlock();
  BasicBlock* epilogue = function.allocateBasicBlock();
  IncomingEdge entry_edge = entry->addSuccessor(loop);
  IncomingEdge loop_edge = loop->addSuccessor(loop);
  loop->addSuccessor(exit);
  exit->addSuccessor(epilogue);

  Instruction* initial =
      entry->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{0});
  Instruction* phi = loop->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  Instruction* next =
      loop->allocateInstr(Opcode::kAdd, nullptr, OutVReg{}, VReg{phi}, Imm{1});
  loop->allocateInstr(
      Opcode::kCondBranch, nullptr, VReg{next}, Lbl{loop}, Lbl{exit});
  phi->addPhiInput(loop_edge, next);
  phi->addPhiInput(entry_edge, initial);
  exit->allocateInstr(Opcode::kReturn, nullptr, VReg{phi});

  LinearScanAllocator allocator{&function};
  allocator.run();

  ASSERT_EQ(phi->numPhiInputs(), 2);
  for (size_t slot = 0; slot < phi->numPhiInputs(); ++slot) {
    const Operand* value = phi->phiInput(slot);
    EXPECT_FALSE(value->isLinked());
    EXPECT_TRUE(value->isReg() || value->isStack());
    EXPECT_EQ(value->instr(), phi);
  }
}

TEST_F(LinearScanAllocatorTest, SpillAllocatorHandlesCriticalEdgePhiSlots) {
  Function function;
  BasicBlock* phi_block = buildCriticalEdgePhiLIR(function);

  ASSERT_EQ(phi_block->numPredecessors(), 2);
  const std::vector<BasicBlock*> predecessors = phi_block->predecessors();

  SpillAllocator allocator{&function};
  allocator.run();

  for (BasicBlock* block : function.basicBlocks()) {
    for (auto& instruction : block->instructions()) {
      EXPECT_FALSE(instruction->isPhi());
    }
  }

  auto last_stack_store = [](const BasicBlock* block) {
    for (auto it = block->instructions().rbegin();
         it != block->instructions().rend();
         ++it) {
      const Instruction* instruction = it->get();
      if (instruction->isStore() && instruction->output()->isStack()) {
        return instruction;
      }
    }
    return static_cast<const Instruction*>(nullptr);
  };
  const Instruction* first_copy = last_stack_store(predecessors[0]);
  const Instruction* second_copy = last_stack_store(predecessors[1]);
  ASSERT_NE(first_copy, nullptr);
  ASSERT_NE(second_copy, nullptr);
  EXPECT_EQ(
      first_copy->output()->getStackSlot(),
      second_copy->output()->getStackSlot());
}

TEST_F(
    LinearScanAllocatorTest,
    ArbitraryExecutionCallReservesAllRegistersInFreeThreadedBuild) {
  hir::Function hir_func;
  hir::Register* dst = hir_func.env.allocateRegister();
  std::unique_ptr<hir::Instr> origin(
      hir::CallInd::create(0, dst, "call_ind", hir::TObject));

  auto lir_func = std::make_unique<Function>();
  BasicBlock* block = lir_func->allocateBasicBlock();
  block->allocateInstr(Opcode::kCall, origin.get(), OutVReg());
  BasicBlock* epilogue = lir_func->allocateBasicBlock();
  block->addSuccessor(epilogue);

  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();

  codegen::PhyRegisterSet expected = codegen::CALLER_SAVE_REGS;
  if constexpr (kFreeThreadedBuild) {
    expected = codegen::INIT_REGISTERS;
  }
  EXPECT_EQ(fixedRegisterIntervals(allocator.fixed_intervals_), expected);
}

TEST_F(LinearScanAllocatorTest, DeoptExitReservesVectorRegisters) {
  auto lir_func = std::make_unique<Function>();
  BasicBlock* block = lir_func->allocateBasicBlock();
  block->allocateInstr(Opcode::kGuard, nullptr);
  BasicBlock* epilogue = lir_func->allocateBasicBlock();
  block->addSuccessor(epilogue);

  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();

  // The deopt trampoline only spills general-purpose registers, so no live
  // value may be in a vector register at a deopt exit.
  EXPECT_EQ(
      fixedRegisterIntervals(allocator.fixed_intervals_),
      codegen::ALL_VECD_REGISTERS);
}

TEST_F(LinearScanAllocatorTest, DeoptExitReservationsNotInChangedRegs) {
  auto lir_func = std::make_unique<Function>();
  BasicBlock* block = lir_func->allocateBasicBlock();
  block->allocateInstr(Opcode::kGuard, nullptr);
  block->allocateInstr(Opcode::kReturn, nullptr);
  BasicBlock* epilogue = lir_func->allocateBasicBlock();
  block->addSuccessor(epilogue);

  LinearScanAllocator allocator(lir_func.get());
  allocator.run();

  // Fixed-interval reservations (e.g. the all-vector reservation at deopt
  // exits) never hold values, so they must not mark registers as modified
  // for prologue/epilogue save/restore.
  EXPECT_TRUE((allocator.getChangedRegs() & codegen::CALLEE_SAVE_REGS).empty());
}

TEST_F(LinearScanAllocatorTest, InoutRegTest) {
  // OptimizeMoveSequence should not set reg operands that are also output
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  auto a = bb->allocateInstr(Opcode::kMove, nullptr, lir::OutVReg(), Imm(0));
  auto b = bb->allocateInstr(Opcode::kMove, nullptr, lir::OutVReg(), Imm(0));

  auto add = bb->allocateInstr(
      Opcode::kAdd, nullptr, lir::OutVReg(), lir::VReg(a), lir::VReg(b));

  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      lir::OutPhyReg{codegen::arch::reg_general_return_loc},
      lir::VReg(add));
  bb->allocateInstr(Opcode::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  runAllocator(lirfunc.get());

  ASSERT_TRUE(
      add->output()->getPhyRegister() == add->getInput(0)->getPhyRegister() ||
      add->output()->getPhyRegister() == add->getInput(1)->getPhyRegister());
}

TEST_F(LinearScanAllocatorTest, CallWithSideEffectTest) {
  // RewriteLIR should not remove function calls
  // since they may have side effects
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  auto a = bb->allocateInstr(Opcode::kCall, nullptr, lir::OutVReg());

  auto b = bb->allocateInstr(Opcode::kMove, nullptr, lir::OutVReg(), Imm(0));

  bb->allocateInstr(
      Opcode::kMove,
      nullptr,
      lir::OutPhyReg{codegen::arch::reg_general_return_loc},
      lir::VReg(b));
  bb->allocateInstr(Opcode::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);
  ASSERT_EQ(a->opcode(), Opcode::kCall);
  ASSERT_EQ(a->output()->type(), lir::Operand::kVreg);
  runAllocator(lirfunc.get());
  ASSERT_EQ(a->opcode(), Opcode::kCall);
  ASSERT_EQ(a->output()->type(), lir::Operand::kNone);
}

} // namespace cinderx::jit::lir
