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
#include "cinderx/Jit/profile.h"
#include "cinderx/Jit/pyjit.h"
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

#ifndef ENABLE_PREFORK_MODEL
TEST_F(BinaryOpCacheCodegenTest, ProfileIndexesBinaryOpsByLocation) {
  PerCompilationStableStorage storage;
  ProfileCollector collector;
  const BCOffset long_offset{24};
  BinaryOpCache* long_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, long_offset, long_cache);
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> long_result = Ref<>::steal(BinaryOpCache::add(one, two, long_cache));
  ASSERT_NE(long_result, nullptr);

  const BCOffset list_offset{48};
  BinaryOpCache* list_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, list_offset, list_cache);
  auto left = Ref<>::steal(PyList_New(0));
  auto right = Ref<>::steal(PyList_New(0));
  Ref<> list_result = Ref<>::steal(BinaryOpCache::add(left, right, list_cache));
  ASSERT_NE(list_result, nullptr);

  const BCOffset cold_offset{60};
  BinaryOpCache* cold_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, cold_offset, cold_cache);

  const BCOffset float_offset{12};
  BinaryOpCache* float_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, float_offset, float_cache);
  auto one_float = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto two_float = Ref<>::steal(PyFloat_FromDouble(2.0));
  Ref<> float_result =
      Ref<>::steal(BinaryOpCache::add(one_float, two_float, float_cache));
  ASSERT_NE(float_result, nullptr);

  const BCOffset unicode_offset{6};
  BinaryOpCache* unicode_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, unicode_offset, unicode_cache);
  auto left_unicode = Ref<>::steal(PyUnicode_FromString("left"));
  auto right_unicode = Ref<>::steal(PyUnicode_FromString("right"));
  Ref<> unicode_result = Ref<>::steal(
      BinaryOpCache::add(left_unicode, right_unicode, unicode_cache));
  ASSERT_NE(unicode_result, nullptr);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 4);

  const BinaryOpProfile* long_profile =
      profile.find<BinaryOpProfile>(nullptr, long_offset);
  ASSERT_NE(long_profile, nullptr);
  EXPECT_EQ(long_profile->left_type, TLongCompact);
  EXPECT_EQ(long_profile->right_type, TLongCompact);

  const BinaryOpProfile* float_profile =
      profile.find<BinaryOpProfile>(nullptr, float_offset);
  ASSERT_NE(float_profile, nullptr);
  EXPECT_EQ(float_profile->left_type, TFloatExact);
  EXPECT_EQ(float_profile->right_type, TFloatExact);

  const BinaryOpProfile* unicode_profile =
      profile.find<BinaryOpProfile>(nullptr, unicode_offset);
  ASSERT_NE(unicode_profile, nullptr);
  EXPECT_EQ(unicode_profile->left_type, TUnicodeExact);
  EXPECT_EQ(unicode_profile->right_type, TUnicodeExact);

  const BinaryOpProfile* list_profile =
      profile.find<BinaryOpProfile>(nullptr, list_offset);
  ASSERT_NE(list_profile, nullptr);
  EXPECT_EQ(list_profile->left_type, TListExact);
  EXPECT_EQ(list_profile->right_type, TListExact);

  // A cache that was never called produces no profile entry.
  EXPECT_EQ(profile.find<BinaryOpProfile>(nullptr, cold_offset), nullptr);

  EXPECT_NE(profile.find<BinaryOpProfile>(nullptr, list_offset), nullptr);
}

#endif

#if !defined(ENABLE_PREFORK_MODEL) && !defined(Py_GIL_DISABLED) && \
    defined(ENABLE_LIGHTWEIGHT_FRAMES) &&                          \
    (defined(__APPLE__) || defined(__linux__))
TEST_F(BinaryOpCacheCodegenTest, RecompileSpecializesFromBinaryOpProfile) {
  const char* src = R"(
def test(a, b):
  return a + b
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);

  auto lhs = Ref<>::steal(PyLong_FromLong(1));
  auto rhs = Ref<>::steal(PyLong_FromLong(2));
  PyObject* args[] = {lhs, rhs};
  auto result = Ref<>::steal(func->vectorcall(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 3);

  Context* ctx = getContext();
  ASSERT_NE(ctx, nullptr);
  BorrowedRef<CompiledFunction> first = ctx->lookupFunc(func);
  ASSERT_NE(first, nullptr);
  Profile profile = first->snapshotProfile();
  ASSERT_EQ(profile.entries().size(), 1);
  const BinaryOpProfile* binary_op_profile =
      profile.find<BinaryOpProfile>(profile.entries().begin()->first);
  ASSERT_NE(binary_op_profile, nullptr);
  EXPECT_EQ(binary_op_profile->left_type, TLongCompact);
  EXPECT_EQ(binary_op_profile->right_type, TLongCompact);

  CompiledFunctionData recompiled = recompileWithProfile(func, profile);
  const hir::OpcodeCounts& counts = recompiled.hir_opcode_counts;
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kBinaryOp)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongBinaryOp)], 1);

  auto recompiled_result = Ref<>::steal(recompiled.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(recompiled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(recompiled_result), 3);
}

TEST_F(BinaryOpCacheCodegenTest, RecompileSpecializesInlinedBinaryOp) {
  // Locations are (code, offset) pairs, so only a single inlined call site is
  // exercised here: two copies of the same inlined BinaryOp would register
  // the same location twice.
  const char* src = R"(
def add(a, b):
  return a + b

def test(a, b):
  return add(a, b)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);

  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  PyObject* args[] = {one, two};
  auto result = Ref<>::steal(func->vectorcall(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 3);

  Context* ctx = getContext();
  ASSERT_NE(ctx, nullptr);
  BorrowedRef<CompiledFunction> first = ctx->lookupFunc(func);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(first->inlinedFunctionsStats().num_inlined_functions, 1);

  Profile profile = first->snapshotProfile();
  ASSERT_EQ(profile.entries().size(), 1);
  const BinaryOpProfile* binary_op_profile =
      profile.find<BinaryOpProfile>(profile.entries().begin()->first);
  ASSERT_NE(binary_op_profile, nullptr);
  EXPECT_EQ(binary_op_profile->left_type, TLongCompact);
  EXPECT_EQ(binary_op_profile->right_type, TLongCompact);

  CompiledFunctionData recompiled = recompileWithProfile(func, profile);
  const hir::OpcodeCounts& counts = recompiled.hir_opcode_counts;
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kBinaryOp)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongBinaryOp)], 1);

  auto recompiled_result = Ref<>::steal(recompiled.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(recompiled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(recompiled_result), 3);
}
#endif

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

TEST_F(BinaryOpCacheCodegenTest, IntThenStrEqualExecuteCorrectly) {
  const char* src = R"(
def test(a, b):
  return a == b
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  // The cache option is handled during LIR generation, so HIR stays generic.
  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc), ::testing::HasSubstr("Compare<Equal>"));

  NativeGeneratorFactory factory;
  NativeGenerator gen(irfunc.get(), factory);
  auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
  ASSERT_NE(jitfunc, nullptr);

  PyObject* self = reinterpret_cast<PyObject*>(funcobj.get());

  // int == int: cold cache specializes to the compact-int fast path.
  {
    auto a = Ref<>::steal(PyLong_FromLong(1));
    auto b = Ref<>::steal(PyLong_FromLong(1));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_True);
  }
  {
    auto a = Ref<>::steal(PyLong_FromLong(1));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_False);
  }

  // str == str on the same compiled function: the int guard fails and the
  // cache falls back to the generic PyObject_RichCompare path.
  {
    auto a = Ref<>::steal(PyUnicode_FromString("x"));
    auto b = Ref<>::steal(PyUnicode_FromString("x"));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_True);
  }

  // int == int again: still correct after the fallback transition.
  {
    auto a = Ref<>::steal(PyLong_FromLong(42));
    auto b = Ref<>::steal(PyLong_FromLong(42));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_True);
  }
}

TEST_F(BinaryOpCacheCodegenTest, IntLessThanExecutesCorrectly) {
  const char* src = R"(
def test(a, b):
  return a < b
)";
  Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
  ASSERT_NE(funcobj, nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(funcobj));
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

  ASSERT_THAT(
      HIRPrinter{}.toString(*irfunc),
      ::testing::HasSubstr("Compare<LessThan>"));

  NativeGeneratorFactory factory;
  NativeGenerator gen(irfunc.get(), factory);
  auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
  ASSERT_NE(jitfunc, nullptr);

  PyObject* self = reinterpret_cast<PyObject*>(funcobj.get());

  {
    auto a = Ref<>::steal(PyLong_FromLong(1));
    auto b = Ref<>::steal(PyLong_FromLong(2));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_True);
  }
  {
    auto a = Ref<>::steal(PyLong_FromLong(2));
    auto b = Ref<>::steal(PyLong_FromLong(1));
    PyObject* args[] = {a, b};
    auto res = Ref<>::steal(jitfunc(self, args, 2, nullptr));
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res.get(), Py_False);
  }
}

} // namespace cinderx
