// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/fixed_type_profiler.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit;

using TypeProfilerTest = RuntimeTest;

TEST_F(TypeProfilerTest, Fixed) {
  Ref<PyTypeObject> a(compileAndGet("class A: pass", "A"));
  ASSERT_NE(a, nullptr);
  Ref<PyTypeObject> b(compileAndGet("class B: pass", "B"));
  ASSERT_NE(b, nullptr);
  Ref<PyTypeObject> c(compileAndGet("class C: pass", "C"));
  ASSERT_NE(c, nullptr);
  FixedTypeProfiler<2> prof;
  ASSERT_TRUE(prof.empty());

  Py_ssize_t a_cnt = Py_REFCNT(a);
  Py_ssize_t b_cnt = Py_REFCNT(b);
  Py_ssize_t c_cnt = Py_REFCNT(c);

  prof.recordType(b);
  prof.recordType(b);
  prof.recordType(a);
  prof.recordType(c);
  prof.recordType(a);
  prof.recordType(c);
  prof.recordType(c);
  prof.recordType(a);
  prof.recordType(c);
  prof.recordType(c);

  ASSERT_EQ(prof.size, 2);
  ASSERT_FALSE(prof.empty());

  EXPECT_EQ(prof.types[0], b);
  EXPECT_EQ(prof.counts[0], 2);
  EXPECT_EQ(prof.types[1], a);
  EXPECT_EQ(prof.counts[1], 3);
  EXPECT_EQ(prof.other, 5);

  EXPECT_GT(Py_REFCNT(a), a_cnt);
  EXPECT_GT(Py_REFCNT(b), b_cnt);
  EXPECT_EQ(Py_REFCNT(c), c_cnt);

  prof.clear();
  ASSERT_TRUE(prof.empty());

  EXPECT_EQ(Py_REFCNT(a), a_cnt);
  EXPECT_EQ(Py_REFCNT(b), b_cnt);
  EXPECT_EQ(Py_REFCNT(c), c_cnt);
}
