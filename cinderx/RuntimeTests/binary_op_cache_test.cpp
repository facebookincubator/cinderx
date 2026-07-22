// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include <gmock/gmock.h>

// clang-format off
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
// clang-format on

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace cinderx::jit;
using namespace cinderx::jit::hir;
using namespace cinderx::jit::codegen;

// End-to-end test of the BinaryOpCached codegen path: enables the binary-op
// inline cache, JIT-compiles a function that does `a + b`, and actually runs
// the generated machine code, asserting the returned value is correct. This
// exercises the direct-call-to-dispatch codegen and the cache's runtime
// state machine (cold -> int-specialized -> generic fallback).
class BinaryOpCacheCodegenTest : public RuntimeTest {
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

  Config saved_config_;
};

TEST_F(BinaryOpCacheCodegenTest, IntThenStrAddExecuteCorrectly) {
  const char* src = R"(
def test(a, b):
  return a + b
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  // The generic add should have been rewritten to the inline-cached variant.
  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc),
      ::testing::HasSubstr("BinaryOpCached<Add>"));

  NativeGeneratorFactory factory;
  NativeGenerator gen(irfunc.get(), factory);
  auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
  ASSERT_NE(jitfunc, nullptr);

  PyObject* self = reinterpret_cast<PyObject*>(funcobj.get());

  // int + int: cold cache specializes to the int fast path (_PyLong_Add).
  {
    auto a = Ref<>::steal(PyLong_FromLong(1));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    auto expected = Ref<>::steal(PyLong_FromLong(3));
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }

  // str + str on the same compiled function: the int guard sees non-ints and
  // permanently falls back to the generic PyNumber_Add path (concatenation).
  {
    auto a = Ref<>::steal(PyUnicode_FromString("x"));
    auto b = Ref<>::steal(PyUnicode_FromString("y"));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    auto expected = Ref<>::steal(PyUnicode_FromString("xy"));
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }

  // int + int again: still correct after the fallback transition.
  {
    auto a = Ref<>::steal(PyLong_FromLong(40));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    auto expected = Ref<>::steal(PyLong_FromLong(42));
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }
}
