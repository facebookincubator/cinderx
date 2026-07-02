// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"

using namespace cinderx::jit::hir;

TEST(BlockCanonicalizerTest, BreaksCycles) {
  CFG cfg;
  Environment env;
  BlockCanonicalizer bc(&env);
  OperandStack stack;

  auto t0 = bc.getOrAllocateCanonicalStack(0);
  auto t1 = bc.getOrAllocateCanonicalStack(1);
  auto t2 = bc.getOrAllocateCanonicalStack(2);

  stack.push(t1);
  stack.push(t2);
  stack.push(t0);

  auto block = cfg.allocateBlock();
  block->append<Return>(env.allocateRegister());

  bc.Run(block, stack);

  HIRPrinter printer;
  const char* expected = R"(bb 0 {
  v4 = Assign v1
  v1 = Assign v2
  v2 = Assign v0
  v0 = Assign v4
  Return v3
}
)";
  ASSERT_EQ(printer.ToString(*block), expected);
}

TEST(BlockCanonicalizerTest, HandlesMultipleOccurrencesOfSingleReg) {
  CFG cfg;
  Environment env;
  BlockCanonicalizer bc(&env);
  OperandStack stack;

  auto t0 = bc.getOrAllocateCanonicalStack(0);
  auto t1 = bc.getOrAllocateCanonicalStack(1);
  auto t2 = bc.getOrAllocateCanonicalStack(2);

  stack.push(t1);
  stack.push(t2);
  stack.push(t0);
  stack.push(t0);
  stack.push(t1);
  stack.push(t1);

  auto block = cfg.allocateBlock();
  block->append<Return>(env.allocateRegister());

  bc.Run(block, stack);

  HIRPrinter printer;
  const char* expected = R"(bb 0 {
  v7 = Assign v1
  v1 = Assign v2
  v2 = Assign v0
  v4 = Assign v0
  v0 = Assign v7
  v5 = Assign v7
  v6 = Assign v7
  Return v3
}
)";
  ASSERT_EQ(printer.ToString(*block), expected);
}

TEST(BlockCanonicalizerTest, HandlesMixOfLocalsAndTemporaries) {
  CFG cfg;
  Environment env;
  BlockCanonicalizer bc(&env);
  OperandStack stack;

  auto t0 = bc.getOrAllocateCanonicalStack(0);
  auto t1 = bc.getOrAllocateCanonicalStack(1);

  auto x = env.allocateRegister();
  auto y = env.allocateRegister();

  stack.push(x);
  stack.push(y);
  stack.push(t0);
  stack.push(t0);
  stack.push(t1);

  auto block = cfg.allocateBlock();
  block->append<Return>(env.allocateRegister());

  bc.Run(block, stack);

  HIRPrinter printer;
  const char* expected = R"(bb 0 {
  v5 = Assign v0
  v6 = Assign v0
  v0 = Assign v2
  v7 = Assign v1
  v1 = Assign v3
  Return v4
}
)";
  ASSERT_EQ(printer.ToString(*block), expected);
}
