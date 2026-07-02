// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <memory>

class LivenessAnalysisTest : public RuntimeTest {};

using namespace cinderx::jit::hir;

TEST_F(LivenessAnalysisTest, SingleBlockHasNoLiveInOut) {
  Function func;
  auto block = func.cfg.allocateBlock();
  func.cfg.entry_block = block;
  auto v0 = func.env.allocateRegister();
  block->append<LoadConst>(v0, TNoneType);
  block->append<Return>(v0);

  LivenessAnalysis liveness(func);
  liveness.run();

  EXPECT_FALSE(liveness.isLiveIn(block, v0));
  EXPECT_FALSE(liveness.isLiveOut(block, v0));
}

TEST_F(LivenessAnalysisTest, UninitializedVariableUseIsLiveIn) {
  // IR looks like:
  //
  // fun empty {
  //   bb 0 {
  //     v0 = LoadArg<0>
  //     CondBranch<1, 2> v0
  //   }
  //
  //   bb 1 {
  //     v1 = LoadConst<NoneType>
  //     Branch<2>
  //   }
  //
  //   bb 2 {
  //     Return v1
  //   }
  // }
  Function func;
  auto entry = func.cfg.allocateBlock();
  func.cfg.entry_block = entry;
  auto v0 = func.env.allocateRegister();
  entry->append<LoadArg>(v0, 0);

  auto t_block = func.cfg.allocateBlock();
  auto f_block = func.cfg.allocateBlock();
  entry->append<CondBranch>(v0, t_block, f_block);

  auto v1 = func.env.allocateRegister();
  t_block->append<LoadConst>(v1, TNoneType);
  t_block->append<Branch>(f_block);

  f_block->append<Return>(v1);

  LivenessAnalysis liveness(func);
  liveness.run();

  // Arguments are killed by the LoadArg pseudo instructions
  EXPECT_FALSE(liveness.isLiveIn(entry, v0));
  EXPECT_FALSE(liveness.isLiveOut(entry, v0));
  // v1 is potentially undefined so it should show up as live-in on entry
  EXPECT_TRUE(liveness.isLiveIn(entry, v1));
  EXPECT_TRUE(liveness.isLiveOut(entry, v1));

  // True block assigns v1, which is used by the return block
  EXPECT_FALSE(liveness.isLiveIn(t_block, v0));
  EXPECT_FALSE(liveness.isLiveOut(t_block, v0));
  EXPECT_FALSE(liveness.isLiveIn(t_block, v1));
  EXPECT_TRUE(liveness.isLiveOut(t_block, v1));

  // Use of v1 in false block is potentially uninitialized
  // No vars should be live out on exit block
  EXPECT_FALSE(liveness.isLiveIn(f_block, v0));
  EXPECT_FALSE(liveness.isLiveOut(f_block, v0));
  EXPECT_TRUE(liveness.isLiveIn(f_block, v1));
  EXPECT_FALSE(liveness.isLiveOut(f_block, v1));
}

TEST_F(LivenessAnalysisTest, PhiUses) {
  Function func;
  auto b0 = func.cfg.entry_block = func.cfg.allocateBlock();
  auto b1 = func.cfg.allocateBlock();
  auto b2 = func.cfg.allocateBlock();

  auto v0 = func.env.allocateRegister();
  auto v1 = func.env.allocateRegister();
  auto v2 = func.env.allocateRegister();
  auto v3 = func.env.allocateRegister();

  b0->append<LoadArg>(v0, 0);
  b0->append<LoadArg>(v1, 1);
  b0->append<LoadArg>(v2, 2);
  b0->append<CondBranch>(v0, b1, b2);

  b1->append<Branch>(b2);

  std::unordered_map<BasicBlock*, Register*> phi_vals{{b1, v1}, {b0, v2}};
  b2->append<Phi>(v3, phi_vals);
  b2->append<Return>(v2);

  ASSERT_TRUE(checkFunc(func, std::cout));

  LivenessAnalysis liveness{func};
  liveness.run();

  EXPECT_FALSE(liveness.isLiveOut(b0, v0));
  EXPECT_TRUE(liveness.isLiveOut(b0, v1));
  EXPECT_TRUE(liveness.isLiveOut(b0, v2));

  EXPECT_TRUE(liveness.isLiveIn(b1, v1));
  EXPECT_TRUE(liveness.isLiveIn(b1, v2));
  EXPECT_FALSE(liveness.isLiveOut(b1, v1));
  EXPECT_TRUE(liveness.isLiveOut(b1, v2));

  EXPECT_FALSE(liveness.isLiveIn(b2, v0));
  EXPECT_FALSE(liveness.isLiveIn(b2, v1));
  EXPECT_TRUE(liveness.isLiveIn(b2, v2));
}

TEST_F(LivenessAnalysisTest, LastUses) {
  Function func;
  auto b0 = func.cfg.entry_block = func.cfg.allocateBlock();
  auto b1 = func.cfg.allocateBlock();
  auto b2 = func.cfg.allocateBlock();
  auto b3 = func.cfg.allocateBlock();

  auto v0 = func.env.allocateRegister();
  auto v1 = func.env.allocateRegister();
  auto v2 = func.env.allocateRegister();
  auto v3 = func.env.allocateRegister();

  FrameState frame;
  b0->append<MakeDict>(v0, 0, frame);
  b0->append<MakeDict>(v1, 0, frame);
  b0->append<MakeDict>(v2, 0, frame);
  b0->append<CondBranch>(v0, b1, b2);

  auto b1_inc = b1->append<Incref>(v1);
  b1->append<Branch>(b3);

  auto b2_inc = b2->append<Incref>(v1);
  auto b2_branch = b2->append<Branch>(b3);

  std::unordered_map<BasicBlock*, Register*> phi_vals{{b1, v2}, {b2, v0}};
  auto phi = b3->append<Phi>(v3, phi_vals);
  auto ret = b3->append<Return>(v2);

  ASSERT_TRUE(checkFunc(func, std::cout));

  LivenessAnalysis liveness{func};
  liveness.run();
  auto last_uses = liveness.getLastUses();
  LivenessAnalysis::LastUses expected_last_uses{
      {b1_inc, {v1}},
      {b2_inc, {v1}},
      {b2_branch, {v0}},
      {phi, {v3}},
      {ret, {v2}},
  };
  EXPECT_EQ(last_uses, expected_last_uses);
}

class DefiniteAssignmentAnalysisTest : public RuntimeTest {};

TEST_F(DefiniteAssignmentAnalysisTest, ArgumentsAlwaysAssigned) {
  Function func;
  auto block = func.cfg.allocateBlock();
  func.cfg.entry_block = block;
  auto v0 = func.env.allocateRegister();
  block->append<LoadArg>(v0, 0);
  block->append<Return>(v0);

  AssignmentAnalysis def_assign(func, true);
  def_assign.run();

  EXPECT_FALSE(def_assign.isAssignedIn(block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(block, v0));
}

TEST_F(
    DefiniteAssignmentAnalysisTest,
    ConditionallyInitializedAreNotDefAssigned) {
  // v1 is assigned along the true branch but not along the false branch.
  // When control flow merges v1 *may* be assigned but is not definitely
  // assigned.
  //
  // fun cond_assign {
  //   bb 0 {
  //     v0 = LoadArg<0>
  //     CondBranch<1, 2> v0
  //   }
  //
  //   bb 1 {
  //     v1 = LoadConst<NoneType>
  //     Branch<2>
  //   }
  //
  //   bb 2 {
  //     x = CheckVar x
  //     Return x
  //   }
  // }
  Function func;
  auto entry = func.cfg.allocateBlock();
  func.cfg.entry_block = entry;
  auto v0 = func.env.allocateRegister();
  entry->append<LoadArg>(v0, 0);

  auto t_block = func.cfg.allocateBlock();
  auto f_block = func.cfg.allocateBlock();
  entry->append<CondBranch>(v0, t_block, f_block);

  auto v1 = func.env.allocateRegister();
  t_block->append<LoadConst>(v1, TNoneType);
  t_block->append<Branch>(f_block);

  f_block->append<Return>(v1);

  AssignmentAnalysis def_assign(func, true);
  def_assign.run();

  EXPECT_FALSE(def_assign.isAssignedIn(entry, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(entry, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(entry, v1));
  EXPECT_FALSE(def_assign.isAssignedOut(entry, v1));

  // True block assigns y
  EXPECT_TRUE(def_assign.isAssignedIn(t_block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(t_block, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(t_block, v1));
  EXPECT_TRUE(def_assign.isAssignedOut(t_block, v1));

  // Since y is only assigned in the true block it should not be assigned on
  // entry to the false block
  EXPECT_TRUE(def_assign.isAssignedIn(f_block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(f_block, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(f_block, v1));
  EXPECT_FALSE(def_assign.isAssignedOut(f_block, v1));
}

TEST_F(DefiniteAssignmentAnalysisTest, CondInitOnAllBranchesAreDefAssigned) {
  // y is assigned in all predecessors, so should be marked as
  // definitely assigned in the exit block
  //
  // fun cond_assign {
  //   bb 0 {
  //     x = LoadArg<0>
  //     CondBranch<1, 2> x
  //   }
  //
  //   bb 1 {
  //     y = LoadConst<NoneType>
  //     Branch<3>
  //   }
  //
  //   bb 2 {
  //     y = LoadConst<NoneType>
  //     Branch<3>
  //   }
  //
  //   bb 3 {
  //     Return y
  //   }
  // }
  Function func;
  auto entry = func.cfg.allocateBlock();
  func.cfg.entry_block = entry;
  auto v0 = func.env.allocateRegister();
  entry->append<LoadArg>(v0, 0);

  auto t_block = func.cfg.allocateBlock();
  auto f_block = func.cfg.allocateBlock();
  entry->append<CondBranch>(v0, t_block, f_block);

  auto exit_block = func.cfg.allocateBlock();
  auto v1 = func.env.allocateRegister();
  t_block->append<LoadConst>(v1, TNoneType);
  t_block->append<Branch>(exit_block);
  f_block->append<LoadConst>(v1, TNoneType);
  f_block->append<Branch>(exit_block);

  exit_block->append<Return>(v1);

  AssignmentAnalysis def_assign(func, true);
  def_assign.run();

  EXPECT_FALSE(def_assign.isAssignedIn(entry, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(entry, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(entry, v1));
  EXPECT_FALSE(def_assign.isAssignedOut(entry, v1));

  // True block assigns v1
  EXPECT_TRUE(def_assign.isAssignedIn(t_block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(t_block, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(t_block, v1));
  EXPECT_TRUE(def_assign.isAssignedOut(t_block, v1));

  // False block assigns y
  EXPECT_TRUE(def_assign.isAssignedIn(f_block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(f_block, v0));
  EXPECT_FALSE(def_assign.isAssignedIn(f_block, v1));
  EXPECT_TRUE(def_assign.isAssignedOut(f_block, v1));

  // y is assigned in both arms of the conditional, so should be marked as
  // definitely assigned on entry to the last block
  EXPECT_TRUE(def_assign.isAssignedIn(exit_block, v0));
  EXPECT_TRUE(def_assign.isAssignedOut(exit_block, v0));
  EXPECT_TRUE(def_assign.isAssignedIn(exit_block, v1));
  EXPECT_TRUE(def_assign.isAssignedOut(exit_block, v1));
}

TEST_F(DefiniteAssignmentAnalysisTest, AssignmentDominatesLoop) {
  // HIR looks like
  // fun test 0 0 0 5 {
  //   bb 0 {
  //     v0 = LoadConst<NoneType>
  //     Branch<1>
  //   }
  //
  //   bb 1 {
  //     CondBranch<2, 3> v0
  //   }
  //
  //   bb 2 {
  //     Branch<4>
  //   }
  //
  //   bb 3 {
  //     v1 = LoadConst<NoneType>
  //     CondBranch<1, 4> v1
  //   }
  //
  //   bb 4 {
  //     Return v0
  //   }
  // }

  Function func;
  auto b0 = func.cfg.allocateBlock();
  auto b1 = func.cfg.allocateBlock();
  auto b2 = func.cfg.allocateBlock();
  auto b3 = func.cfg.allocateBlock();
  auto b4 = func.cfg.allocateBlock();

  func.cfg.entry_block = b0;
  auto v0 = func.env.allocateRegister();
  b0->append<LoadConst>(v0, TNoneType);
  b0->append<Branch>(b1);
  b1->append<CondBranch>(v0, b2, b3);
  b2->append<Branch>(b4);
  auto v1 = func.env.allocateRegister();
  b3->append<LoadConst>(v1, TNoneType);
  b3->append<CondBranch>(v1, b1, b4);
  b4->append<Return>(v0);

  AssignmentAnalysis assign(func, true);
  assign.run();

  EXPECT_FALSE(assign.isAssignedIn(b0, v0));
  EXPECT_FALSE(assign.isAssignedIn(b0, v1));
  EXPECT_TRUE(assign.isAssignedOut(b0, v0));
  EXPECT_FALSE(assign.isAssignedOut(b0, v1));

  EXPECT_TRUE(assign.isAssignedIn(b1, v0));
  EXPECT_FALSE(assign.isAssignedIn(b1, v1));
  EXPECT_TRUE(assign.isAssignedOut(b1, v0));
  EXPECT_FALSE(assign.isAssignedOut(b1, v1));

  EXPECT_TRUE(assign.isAssignedIn(b2, v0));
  EXPECT_FALSE(assign.isAssignedIn(b2, v1));
  EXPECT_TRUE(assign.isAssignedOut(b2, v0));
  EXPECT_FALSE(assign.isAssignedOut(b2, v1));

  EXPECT_TRUE(assign.isAssignedIn(b3, v0));
  EXPECT_FALSE(assign.isAssignedIn(b3, v1));
  EXPECT_TRUE(assign.isAssignedOut(b3, v0));
  EXPECT_TRUE(assign.isAssignedOut(b3, v1));

  EXPECT_TRUE(assign.isAssignedIn(b4, v0));
  EXPECT_FALSE(assign.isAssignedIn(b4, v1));
  EXPECT_TRUE(assign.isAssignedOut(b4, v0));
  EXPECT_FALSE(assign.isAssignedOut(b4, v1));
}

class DominatorAnalysisTest : public RuntimeTest {};

TEST_F(DominatorAnalysisTest, CorrectlyComputesDiamondCFG) {
  const char* src = R"(
fun dominators {
   bb 0 {
     v0 = LoadArg<0>
     CondBranch<1, 2> v0
   }

   bb 1 {
     Branch<3>
   }

   bb 2 {
     Branch<3>
   }

   bb 3 {
     Return v0
   }
 }
)";
  std::unique_ptr<Function> func = HIRParser().parseHIR(src);

  auto bb0 = func->cfg.getBlockById(0);
  auto bb1 = func->cfg.getBlockById(1);
  auto bb2 = func->cfg.getBlockById(2);
  auto bb3 = func->cfg.getBlockById(3);

  DominatorAnalysis doms(*func);

  EXPECT_EQ(doms.immediateDominator(bb0), nullptr);
  EXPECT_EQ(doms.immediateDominator(bb1), bb0);
  EXPECT_EQ(doms.immediateDominator(bb2), bb0);
  EXPECT_EQ(doms.immediateDominator(bb3), bb0);
}

TEST_F(DominatorAnalysisTest, CorrectlyComputesComplexCFG) {
  const char* src = R"(
fun dominators {
   bb 0 {
     v0 = LoadArg<0>
     Branch<1>
   }

   bb 1 {
     CondBranch<2, 4> v0
   }

   bb 2 {
     Branch<3>
   }

   bb 3 {
     CondBranch<5, 7> v0
   }

   bb 4 {
     Branch<5>
   }

   bb 5 {
     Branch<6>
   }

   bb 6 {
     Branch<7>
   }

   bb 7 {
     Return v0
   }
 }
)";
  std::unique_ptr<Function> func = HIRParser().parseHIR(src);

  auto bb0 = func->cfg.getBlockById(0);
  auto bb1 = func->cfg.getBlockById(1);
  auto bb2 = func->cfg.getBlockById(2);
  auto bb3 = func->cfg.getBlockById(3);
  auto bb4 = func->cfg.getBlockById(4);
  auto bb5 = func->cfg.getBlockById(5);
  auto bb6 = func->cfg.getBlockById(6);
  auto bb7 = func->cfg.getBlockById(7);

  DominatorAnalysis doms(*func);

  EXPECT_EQ(doms.immediateDominator(bb0), nullptr);
  EXPECT_EQ(doms.immediateDominator(bb1), bb0);
  EXPECT_EQ(doms.immediateDominator(bb2), bb1);
  EXPECT_EQ(doms.immediateDominator(bb3), bb2);
  EXPECT_EQ(doms.immediateDominator(bb4), bb1);
  EXPECT_EQ(doms.immediateDominator(bb5), bb1);
  EXPECT_EQ(doms.immediateDominator(bb6), bb5);
  EXPECT_EQ(doms.immediateDominator(bb7), bb1);

  std::unordered_set<const BasicBlock*> dommed = doms.getBlocksDominatedBy(bb7);
  EXPECT_EQ(dommed.size(), 1);
  EXPECT_TRUE(dommed.contains(bb7));

  dommed = doms.getBlocksDominatedBy(bb6);
  EXPECT_EQ(dommed.size(), 1);
  EXPECT_TRUE(dommed.contains(bb6));

  dommed = doms.getBlocksDominatedBy(bb5);
  EXPECT_EQ(dommed.size(), 2);
  EXPECT_TRUE(dommed.contains(bb5));
  EXPECT_TRUE(dommed.contains(bb6));

  dommed = doms.getBlocksDominatedBy(bb4);
  EXPECT_EQ(dommed.size(), 1);
  EXPECT_TRUE(dommed.contains(bb4));

  dommed = doms.getBlocksDominatedBy(bb3);
  EXPECT_EQ(dommed.size(), 1);
  EXPECT_TRUE(dommed.contains(bb3));

  dommed = doms.getBlocksDominatedBy(bb2);
  EXPECT_EQ(dommed.size(), 2);
  EXPECT_TRUE(dommed.contains(bb2));
  EXPECT_TRUE(dommed.contains(bb3));

  dommed = doms.getBlocksDominatedBy(bb1);
  EXPECT_EQ(dommed.size(), 7);
  EXPECT_TRUE(dommed.contains(bb1));
  EXPECT_TRUE(dommed.contains(bb2));
  EXPECT_TRUE(dommed.contains(bb3));
  EXPECT_TRUE(dommed.contains(bb4));
  EXPECT_TRUE(dommed.contains(bb5));
  EXPECT_TRUE(dommed.contains(bb6));
  EXPECT_TRUE(dommed.contains(bb7));

  dommed = doms.getBlocksDominatedBy(bb0);
  EXPECT_EQ(dommed.size(), 8);
  EXPECT_TRUE(dommed.contains(bb0));
  EXPECT_TRUE(dommed.contains(bb1));
  EXPECT_TRUE(dommed.contains(bb2));
  EXPECT_TRUE(dommed.contains(bb3));
  EXPECT_TRUE(dommed.contains(bb4));
  EXPECT_TRUE(dommed.contains(bb5));
  EXPECT_TRUE(dommed.contains(bb6));
  EXPECT_TRUE(dommed.contains(bb7));
}

class RegisterTypeHintsTest : public RuntimeTest {};

TEST_F(RegisterTypeHintsTest, CorrectlyIdentifiesDominatingHintType) {
  const char* src = R"(
fun type_hints {
   bb 0 {
     v0 = LoadArg<0>
     Branch<1>
   }

   bb 1 {
     HintType<1, <Tuple>, <List>> v0
     CondBranch<2, 4> v0
   }

   bb 2 {
     HintType<1, <Tuple>> v0
     Branch<3>
   }

   bb 3 {
     CondBranch<5, 7> v0
   }

   bb 4 {
     HintType<1, <List>> v0
     v1 = LoadArg<1>
     Branch<5>
   }

   bb 5 {
     v2 = Phi<3, 4> v0 v1
     Branch<6>
   }

   bb 6 {
     Branch<7>
   }

   bb 7 {
     Return v0
   }
 }
)";
  std::unique_ptr<Function> func = HIRParser().parseHIR(src);

  auto bb0 = func->cfg.getBlockById(0);
  const Instr& v0_load = bb0->front();
  Register* v0 = v0_load.output();

  auto bb1 = func->cfg.getBlockById(1);
  const Instr& bb1_hint = bb1->front();

  auto bb2 = func->cfg.getBlockById(2);
  const Instr& bb2_hint = bb2->front();

  auto bb3 = func->cfg.getBlockById(3);

  auto bb4 = func->cfg.getBlockById(4);
  auto bb4_iter = bb4->begin();
  const Instr& bb4_hint = *(bb4_iter++);
  const Instr& bb4_load = *bb4_iter;
  Register* v1 = bb4_load.output();

  auto bb5 = func->cfg.getBlockById(5);
  const Instr& bb5_phi = bb5->front();
  Register* v2 = bb5_phi.output();

  auto bb6 = func->cfg.getBlockById(6);

  auto bb7 = func->cfg.getBlockById(7);

  RegisterTypeHints seen(*func);

  EXPECT_EQ(seen.dominatingTypeHint(v0, bb0), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb1), &bb1_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb2), &bb2_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb3), &bb2_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb4), &bb4_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb5), &bb1_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb5), &bb1_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb6), &bb1_hint);
  EXPECT_EQ(seen.dominatingTypeHint(v0, bb7), &bb1_hint);

  EXPECT_EQ(seen.dominatingTypeHint(v2, bb0), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb1), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb2), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb3), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb4), nullptr);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb5), &bb5_phi);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb6), &bb5_phi);
  EXPECT_EQ(seen.dominatingTypeHint(v2, bb7), nullptr);

  EXPECT_EQ(seen.dominatingTypeHint(v1, bb4), nullptr);
}
