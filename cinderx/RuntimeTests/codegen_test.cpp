// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace cinderx::jit::codegen;

namespace cinderx::jit::codegen {

class CodegenTest : public RuntimeTest {};

TEST_F(CodegenTest, TestPhyRegisterSet) {
  auto set = PhyRegisterSet(2) | PhyRegisterSet(3) | PhyRegisterSet(5);

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 3);
  ASSERT_EQ(set.getFirst(), 2);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_EQ(set.has(3), true);

  set.removeFirst();

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 2);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 5);
  ASSERT_EQ(set.has(3), true);

  set.removeLast();

  ASSERT_EQ(set.empty(), false);
  ASSERT_EQ(set.count(), 1);
  ASSERT_EQ(set.getFirst(), 3);
  ASSERT_EQ(set.getLast(), 3);
  ASSERT_EQ(set.has(3), true);

  set.removeFirst();

  ASSERT_EQ(set.empty(), true);
  ASSERT_EQ(set.count(), 0);
  ASSERT_EQ(set.has(3), false);
}

} // namespace cinderx::jit::codegen
