// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit::codegen;

namespace jit::codegen {

class CodegenTest : public RuntimeTest {};

TEST_F(CodegenTest, TestPhyRegisterSet) {
  auto set = PhyRegisterSet(2) | PhyRegisterSet(3) | PhyRegisterSet(5);

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 3);
  ASSERT_EQ(set.GetFirst(), 2);
  ASSERT_EQ(set.GetLast(), 5);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveFirst();

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 2);
  ASSERT_EQ(set.GetFirst(), 3);
  ASSERT_EQ(set.GetLast(), 5);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveLast();

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 1);
  ASSERT_EQ(set.GetFirst(), 3);
  ASSERT_EQ(set.GetLast(), 3);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveFirst();

  ASSERT_EQ(set.Empty(), true);
  ASSERT_EQ(set.count(), 0);
  ASSERT_EQ(set.Has(3), false);
}

} // namespace jit::codegen
