// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include "cinderx/Jit/config.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <gmock/gmock.h>

using namespace cinderx::jit;
using namespace cinderx::jit::hir;

// Fixture that enables the (off-by-default) binary-op inline cache and restores
// the JIT config afterwards so other tests are unaffected.
class SimplifyBinaryOpCacheTest : public RuntimeTest {
 protected:
  void SetUp() override {
    RuntimeTest::SetUp();
    saved_config_ = getConfig();
    getMutableConfig().binary_op_caches = true;
  }

  void TearDown() override {
    getMutableConfig() = saved_config_;
    RuntimeTest::TearDown();
  }

  std::string runSimplify(const char* hir) {
    auto irfunc = HIRParser{}.parseHIR(hir);
    if (irfunc == nullptr) {
      return "<parse failed>";
    }
    reflowTypes(*irfunc);
    Simplify{}.run(*irfunc);
    return HIRPrinter{}.toString(*irfunc);
  }

  Config saved_config_;
};

// A generic add of two unknown-typed objects should be rewritten into the
// inline-cached BinaryOpCached variant when the cache is enabled.
TEST_F(SimplifyBinaryOpCacheTest, GenericAddBecomesBinaryOpCached) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = BinaryOp<Add> v0 v1
    Return v2
  }
}
)";
  EXPECT_THAT(runSimplify(hir), ::testing::HasSubstr("BinaryOpCached<Add>"));
}

// Non-add operations are not affected by the binary-op cache.
TEST_F(SimplifyBinaryOpCacheTest, GenericSubtractStaysBinaryOp) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = BinaryOp<Subtract> v0 v1
    Return v2
  }
}
)";
  std::string out = runSimplify(hir);
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("BinaryOpCached")));
  EXPECT_THAT(out, ::testing::HasSubstr("BinaryOp<Subtract>"));
}

// When the cache is disabled (the default), a generic add is left untouched.
TEST_F(SimplifyBinaryOpCacheTest, GenericAddStaysBinaryOpWhenDisabled) {
  getMutableConfig().binary_op_caches = false;
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = BinaryOp<Add> v0 v1
    Return v2
  }
}
)";
  std::string out = runSimplify(hir);
  EXPECT_THAT(out, ::testing::Not(::testing::HasSubstr("BinaryOpCached")));
  EXPECT_THAT(out, ::testing::HasSubstr("BinaryOp<Add>"));
}
