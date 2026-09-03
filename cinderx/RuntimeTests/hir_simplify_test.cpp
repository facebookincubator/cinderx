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

namespace cinderx {

using namespace cinderx::jit;
using namespace cinderx::jit::hir;

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

// Binary-op caching is selected during LIR generation, not HIR simplification.
TEST_F(SimplifyBinaryOpCacheTest, GenericAddStaysBinaryOpWhenCacheEnabled) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = BinaryOp<Add> v0 v1
    Return v2
  }
}
)";
  EXPECT_THAT(runSimplify(hir), ::testing::HasSubstr("BinaryOp<Add>"));
}

TEST_F(
    SimplifyBinaryOpCacheTest,
    GenericMultiplyStaysBinaryOpWhenCacheEnabled) {
  const char* hir = R"(fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = BinaryOp<Multiply> v0 v1
    Return v2
  }
}
)";
  EXPECT_THAT(runSimplify(hir), ::testing::HasSubstr("BinaryOp<Multiply>"));
}

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
  EXPECT_THAT(runSimplify(hir), ::testing::HasSubstr("BinaryOp<Subtract>"));
}

TEST_F(SimplifyBinaryOpCacheTest, GenericAddStaysBinaryOpWhenCacheDisabled) {
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
  EXPECT_THAT(runSimplify(hir), ::testing::HasSubstr("BinaryOp<Add>"));
}

} // namespace cinderx
