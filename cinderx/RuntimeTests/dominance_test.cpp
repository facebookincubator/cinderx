// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/dominance.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace cinderx::jit::hir;

class DominanceTest : public RuntimeTest {};

TEST_F(DominanceTest, CorrectlyComputesDiamondCFG) {
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

  DominatorTree doms{*func};

  EXPECT_EQ(doms.immediateDominator(bb0), nullptr);
  EXPECT_EQ(doms.immediateDominator(bb1), bb0);
  EXPECT_EQ(doms.immediateDominator(bb2), bb0);
  EXPECT_EQ(doms.immediateDominator(bb3), bb0);
}

TEST_F(DominanceTest, CorrectlyComputesComplexCFG) {
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

  DominatorTree doms{*func};

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
