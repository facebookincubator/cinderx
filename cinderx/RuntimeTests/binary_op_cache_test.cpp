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

namespace cinderx {

using namespace cinderx::jit;
using namespace cinderx::jit::hir;
using namespace cinderx::jit::codegen;

// End-to-end test of the cached BinaryOp codegen path: enables the binary-op
// inline cache, JIT-compiles a function that does `a + b`, and exercises the
// cache's runtime state machine (cold -> int-specialized -> generic fallback).
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

  // The cache option is handled during LIR generation, so HIR stays generic.
  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc), ::testing::HasSubstr("BinaryOp<Add>"));

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

TEST_F(BinaryOpCacheCodegenTest, IntThenSetSubtractExecuteCorrectly) {
  const char* src = R"(
def test(a, b):
  return a - b
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  // The cache option is handled during LIR generation, so HIR stays generic.
  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc),
      ::testing::HasSubstr("BinaryOp<Subtract>"));

  NativeGeneratorFactory factory;
  NativeGenerator gen(irfunc.get(), factory);
  auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
  ASSERT_NE(jitfunc, nullptr);

  PyObject* self = reinterpret_cast<PyObject*>(funcobj.get());

  // int - int: cold cache specializes to the compact-int fast path.
  {
    auto a = Ref<>::steal(PyLong_FromLong(10));
    auto b = Ref<>::steal(PyLong_FromLong(4));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    auto expected = Ref<>::steal(PyLong_FromLong(6));
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }

  // set - set on the same compiled function: the int guard sees non-ints and
  // permanently falls back to the generic PyNumber_Subtract path (difference).
  {
    auto a = Ref<>::steal(PySet_New(nullptr));
    auto b = Ref<>::steal(PySet_New(nullptr));
    auto expected = Ref<>::steal(PySet_New(nullptr));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(expected, nullptr);
    for (long value : {1, 2, 3}) {
      auto item = Ref<>::steal(PyLong_FromLong(value));
      ASSERT_EQ(PySet_Add(a, item), 0);
    }
    auto two = Ref<>::steal(PyLong_FromLong(2));
    ASSERT_EQ(PySet_Add(b, two), 0);
    for (long value : {1, 3}) {
      auto item = Ref<>::steal(PyLong_FromLong(value));
      ASSERT_EQ(PySet_Add(expected, item), 0);
    }

    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }

  // int - int again: still correct after the fallback transition.
  {
    auto a = Ref<>::steal(PyLong_FromLong(44));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    auto expected = Ref<>::steal(PyLong_FromLong(42));
    EXPECT_EQ(PyObject_RichCompareBool(res, expected, Py_EQ), 1);
  }
}

TEST_F(BinaryOpCacheCodegenTest, FloatThenIntTrueDivideExecuteCorrectly) {
  const char* src = R"(
def test(a, b):
  return a / b
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  // CPython has no BINARY_OP_TRUE_DIVIDE specialization, so nothing upstream
  // types these operands and the op reaches LIR generation still generic.
  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc),
      ::testing::HasSubstr("BinaryOp<TrueDivide>"));

  NativeGeneratorFactory factory;
  NativeGenerator gen(irfunc.get(), factory);
  auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
  ASSERT_NE(jitfunc, nullptr);

  PyObject* self = reinterpret_cast<PyObject*>(funcobj.get());

  // float / float: cold cache specializes to the float fast path.
  {
    auto a = Ref<>::steal(PyFloat_FromDouble(1.0));
    auto b = Ref<>::steal(PyFloat_FromDouble(4.0));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    ASSERT_TRUE(PyFloat_CheckExact(res));
    EXPECT_EQ(PyFloat_AsDouble(res), 0.25);
  }

  // int / int on the same compiled function: the float guard fails, the cache
  // falls back to generic, and the result is still a float.
  {
    auto a = Ref<>::steal(PyLong_FromLong(7));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    ASSERT_TRUE(PyFloat_CheckExact(res));
    EXPECT_EQ(PyFloat_AsDouble(res), 3.5);
  }

  // Division by zero raises rather than returning inf.
  {
    auto a = Ref<>::steal(PyLong_FromLong(1));
    auto b = Ref<>::steal(PyLong_FromLong(0));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    EXPECT_EQ(res, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();
  }

  // float / float again: still correct after the fallback transition.
  {
    auto a = Ref<>::steal(PyFloat_FromDouble(9.0));
    auto b = Ref<>::steal(PyFloat_FromDouble(2.0));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(PyFloat_AsDouble(res), 4.5);
  }
}

} // namespace cinderx
