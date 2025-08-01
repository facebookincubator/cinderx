// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"

class SanityTest : public RuntimeTest {};

TEST_F(SanityTest, CanUsePrivateAPIs) {
  auto g = Ref<>::steal(PyLong_FromLong(100));
  ASSERT_NE(g.get(), nullptr);
  ASSERT_TRUE(PyLong_CheckExact(g.get()));
  ASSERT_EQ(PyLong_AsInt(g.get()), 100);
}

TEST_F(SanityTest, CanReinitRuntime) {
  TearDown();
  SetUp();
}
