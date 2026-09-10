// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX > 0x030E0000
#include <pycore_unicodeobject.h>
#endif

#include <cmath>
#include <cstring>

namespace cinderx {

using namespace cinderx::jit;
using namespace cinderx::jit::hir;

class InlineCacheTest : public RuntimeTest {};

#ifndef ENABLE_PREFORK_MODEL
namespace {

struct InlineCacheLifetime {
  explicit InlineCacheLifetime(int& count) : live_count{count} {
    live_count++;
  }

  ~InlineCacheLifetime() noexcept {
    live_count--;
  }

  int& live_count;
};

} // namespace

TEST_F(InlineCacheTest, CompiledDataOwnsAndReclaimsDiscoverableInlineCaches) {
  int live_count = 0;
  {
    CompiledFunctionData original;
    original.inline_cache_storage =
        std::make_unique<PerCompilationInlineCacheStorage>();
    original.inline_cache_storage->allocateForTesting<InlineCacheLifetime>(
        live_count);

    const BCOffset bytecode_offset{24};
    BinaryOpCache* cache = original.inline_cache_storage->allocateBinaryOpCache(
        bytecode_offset, BinaryOpKind::kAdd);

    CompiledFunctionData moved{std::move(original)};

    const std::vector<InlineCacheSite>& sites =
        moved.inline_cache_storage->inlineCacheSites();
    ASSERT_EQ(sites.size(), 1);
    EXPECT_EQ(sites.front().kind, InlineCacheSite::Kind::kBinaryOp);
    EXPECT_EQ(sites.front().bytecode_offset, bytecode_offset);
    EXPECT_EQ(sites.front().cache.binary_op, cache);
    EXPECT_EQ(live_count, 1);
  }
  EXPECT_EQ(live_count, 0);
}

TEST_F(InlineCacheTest, DeferredCompiledDataContributesInlineCacheStats) {
  Ref<PyFunctionObject> func(compileStockAndGet("def func(): pass\n", "func"));
  Context* context = getContext();
  ASSERT_NE(context, nullptr);

  CompiledFunctionData data;
  data.runtime = context->allocateCodeRuntime(func);
  data.inline_cache_storage =
      std::make_unique<PerCompilationInlineCacheStorage>();
  LoadMethodCache* cache =
      data.inline_cache_storage->allocateLoadMethodCache(BCOffset{24});
  cache->initCacheStats("deferred.py", "func");

  {
    Ref<CompiledFunction> compiled =
        CompiledFunction::create(std::move(data), false);
    ASSERT_NE(compiled, nullptr);
    compiled->setOwner(context);
  }

  InlineCacheStats stats = context->getAndClearLoadMethodCacheStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats.front().filename, "deferred.py");
  EXPECT_EQ(stats.front().method_name, "func");
}
#endif

TEST_F(InlineCacheTest, LoadTypeMethodCacheLookUp) {
  const char* src = R"(
from abc import ABCMeta, abstractmethod

class RequestContext:

  @classmethod
  def class_meth(cls):
    pass

  @staticmethod
  def static_meth():
    pass

  def regular_meth():
    pass

class_meth = RequestContext.class_meth.__func__
static_meth = RequestContext.static_meth
regular_meth = RequestContext.regular_meth
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* klass = PyDict_GetItemString(locals, "RequestContext");
  ASSERT_NE(klass, nullptr) << "Couldn't get class RequestContext";

  auto py_class_meth = Ref<>::steal(PyUnicode_FromString("class_meth"));
  LoadTypeMethodCache cache;
  auto res = cache.lookup(klass, py_class_meth);
  ASSERT_EQ(res.self_or_null, klass)
      << "Expected instance to be equal to class from cache look up";
  PyObject* class_meth = PyDict_GetItemString(locals, "class_meth");
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, class_meth, Py_EQ), 1)
      << "Expected method " << class_meth << " to be equal from cache lookup";
  ASSERT_EQ(cache.value(), res.callable)
      << "Expected method " << py_class_meth << " to be cached";

  for (auto& meth : {"static_meth", "regular_meth"}) {
    auto name = Ref<>::steal(PyUnicode_FromString(meth));
    LoadTypeMethodCache methCache;
    auto methRes = methCache.lookup(klass, name);
    PyObject* py_meth = PyDict_GetItemString(locals, meth);
#if PY_VERSION_HEX < 0x030E0000
    ASSERT_EQ(methRes.callable, Py_None)
        << "Expected first part of cache result to be Py_None";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.self_or_null, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.self_or_null)
        << "Expected method " << meth << " to be cached";
#else
    ASSERT_EQ(methRes.self_or_null, nullptr)
        << "Expected first part of cache result to be nullptr";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.callable, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.callable)
        << "Expected method " << meth << " to be cached";
#endif
  }
}

TEST_F(InlineCacheTest, LoadModuleMethodCacheLookUp) {
  const char* src = R"(
import functools
module_meth = functools._unwrap_partial
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* functools_mod = PyDict_GetItemString(locals, "functools");
  ASSERT_NE(functools_mod, nullptr) << "Couldn't get module functools";

  PyObject* module_meth = PyDict_GetItemString(locals, "module_meth");
  ASSERT_NE(module_meth, nullptr) << "Couldn't get PyObject module_meth";

  PyObject* name_obj = PyUnicode_FromString("_unwrap_partial");
  ASSERT_NE(name_obj, nullptr) << "Couldn't create name object";
#if PY_VERSION_HEX >= 0x030E0000
  _PyUnicode_InternImmortal(PyInterpreterState_Get(), &name_obj);
#endif
  auto name = Ref<>::steal(name_obj);

  LoadModuleMethodCache cache;
  auto res = cache.lookup(functools_mod, name);
#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(res.self_or_null, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(Py_None, res.callable)
      << "Expected Py_None to be returned from cache lookup";
#else
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(nullptr, res.self_or_null)
      << "Expected nullptr to be returned in self_or_null from cache lookup";
#endif

#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(cache.value(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#else
  ASSERT_EQ(PyObject_RichCompareBool(*cache.cache(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#endif
  ASSERT_EQ(
      PyObject_RichCompareBool(cache.moduleObj(), functools_mod, Py_EQ), 1)
      << "Expected functools to be cached as an obj";
}

TEST_F(InlineCacheTest, BinaryOpCacheIntAddSpecializes) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{hir::BinaryOpKind::kAdd};
  BinaryOpCache::BinarySpecialization initial = cache.specializedTypes();

  auto lhs = Ref<>::steal(PyLong_FromLong(3));
  auto rhs = Ref<>::steal(PyLong_FromLong(4));
  ASSERT_NE(lhs.get(), nullptr);
  ASSERT_NE(rhs.get(), nullptr);

  auto result = Ref<>::steal(BinaryOpCache::add(lhs, rhs, &cache));
  ASSERT_NE(result.get(), nullptr) << "int + int should succeed";
  EXPECT_EQ(PyLong_AsLong(result), 7);

  // The cache should have specialized away from the initial populate state to
  // the compact-long fast path.
  EXPECT_NE(cache.specializedTypes(), initial);
  BinaryOpCache::BinarySpecialization specialized = cache.specializedTypes();
  EXPECT_EQ(specialized.lhs, SpecializedType::kCompactLong);

  // A subsequent int + int call keeps using the same specialization.
  auto result2 = Ref<>::steal(BinaryOpCache::add(lhs, rhs, &cache));
  ASSERT_NE(result2.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(result2), 7);
  EXPECT_EQ(cache.specializedTypes(), specialized);
}

TEST_F(InlineCacheTest, BinaryOpCacheNonIntUsesGeneric) {
  BinaryOpCache cache{hir::BinaryOpKind::kAdd};

  auto lhs = Ref<>::steal(PyFloat_FromDouble(1.5));
  auto rhs = Ref<>::steal(PyFloat_FromDouble(2.5));
  ASSERT_NE(lhs.get(), nullptr);
  ASSERT_NE(rhs.get(), nullptr);

  auto result = Ref<>::steal(BinaryOpCache::add(lhs, rhs, &cache));
  ASSERT_NE(result.get(), nullptr) << "float + float should succeed";
  ASSERT_TRUE(PyFloat_CheckExact(result));
  EXPECT_EQ(PyFloat_AsDouble(result), 4.0);
}

TEST_F(InlineCacheTest, BinaryOpCacheIntThenNonIntFallsBack) {
  BinaryOpCache cache{hir::BinaryOpKind::kAdd};

  auto i1 = Ref<>::steal(PyLong_FromLong(10));
  auto i2 = Ref<>::steal(PyLong_FromLong(20));
  // First specialize on ints.
  auto int_result = Ref<>::steal(BinaryOpCache::add(i1, i2, &cache));
  ASSERT_NE(int_result.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(int_result), 30);
  BinaryOpCache::BinarySpecialization int_spec = cache.specializedTypes();

  // Now call with floats: the int-guard should fall back to the generic path
  // and permanently change the specialization.
  auto f1 = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto f2 = Ref<>::steal(PyFloat_FromDouble(2.0));
  auto float_result = Ref<>::steal(BinaryOpCache::add(f1, f2, &cache));
  ASSERT_NE(float_result.get(), nullptr) << "float + float should succeed";
  ASSERT_TRUE(PyFloat_CheckExact(float_result));
  EXPECT_EQ(PyFloat_AsDouble(float_result), 3.0);
  EXPECT_NE(cache.specializedTypes(), int_spec)
      << "Mixed types should step away from the int-specialized state";
}

namespace {
using BinaryOpDispatch = PyObject* (*)(PyObject * lhs,
                                       PyObject* rhs,
                                       BinaryOpCache* cache);

// Runs lhs `op` rhs through the cache via the given dispatch entry point
// (BinaryOpCache::add or ::multiply) and returns the (lhs, rhs, return) types
// it settled on, as reported by specializedTypes().
BinaryOpCache::BinarySpecialization specializeWith(
    BinaryOpDispatch dispatch,
    BinaryOpCache& cache,
    PyObject* lhs,
    PyObject* rhs) {
  Ref<>::steal(dispatch(lhs, rhs, &cache));
  return cache.specializedTypes();
}

// Shorthand for a specialization whose lhs, rhs and return types are all
// `kind`.
BinaryOpCache::BinarySpecialization sameTypes(SpecializedType kind) {
  return {kind, kind, kind};
}

// Shorthand for a specialization with explicit lhs, rhs and return types.
BinaryOpCache::BinarySpecialization
types(SpecializedType lhs, SpecializedType rhs, SpecializedType ret) {
  return {lhs, rhs, ret};
}

// Runs lhs / rhs through a fresh cache and asserts the result is identical to
// what PyNumber_TrueDivide produces -- same type, same bits, same sign of zero.
// This is what pins the hand-rolled compact-int fast path to CPython's
// correctly-rounded long_true_divide.
void expectTrueDivideMatchesPyNumber(PyObject* lhs, PyObject* rhs) {
  BinaryOpCache cache{BinaryOpKind::kTrueDivide};
  auto cached = Ref<>::steal(BinaryOpCache::trueDivide(lhs, rhs, &cache));
  auto oracle = Ref<>::steal(PyNumber_TrueDivide(lhs, rhs));
  ASSERT_NE(cached.get(), nullptr);
  ASSERT_NE(oracle.get(), nullptr);
  ASSERT_EQ(Py_TYPE(cached.get()), Py_TYPE(oracle.get()));
  double cached_value = PyFloat_AsDouble(cached);
  double oracle_value = PyFloat_AsDouble(oracle);
  if (std::isnan(oracle_value)) {
    // NaN never compares equal to itself, and its sign bit is not meaningful.
    EXPECT_TRUE(std::isnan(cached_value));
    return;
  }
  EXPECT_EQ(cached_value, oracle_value);
  EXPECT_EQ(std::signbit(cached_value), std::signbit(oracle_value));
}

// Builds an exact set of ints, or nullptr if any step fails.
Ref<> makeSet(std::initializer_list<long> values) {
  auto set = Ref<>::steal(PySet_New(nullptr));
  if (set == nullptr) {
    return nullptr;
  }
  for (long value : values) {
    auto item = Ref<>::steal(PyLong_FromLong(value));
    if (item == nullptr || PySet_Add(set, item) < 0) {
      return nullptr;
    }
  }
  return set;
}
} // namespace

TEST_F(InlineCacheTest, BinaryOpCacheSpecializationLookup) {
  using SpecializedType = SpecializedType;

  // A fresh cache has not specialized yet.
  BinaryOpCache fresh{BinaryOpKind::kAdd};
  EXPECT_EQ(
      fresh.specializedTypes(), sameTypes(SpecializedType::kUninitialized));

  // Small ints fit in a single digit -> compact-long SpecializedType.
  auto small = Ref<>::steal(PyLong_FromLong(3));
  BinaryOpCache compact_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, compact_cache, small, small),
      sameTypes(SpecializedType::kCompactLong));

  // Large ints span multiple digits -> general long SpecializedType.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  BinaryOpCache long_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, long_cache, big, big),
      sameTypes(SpecializedType::kLong));

  auto str = Ref<>::steal(PyUnicode_FromString("x"));
  BinaryOpCache unicode_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, unicode_cache, str, str),
      sameTypes(SpecializedType::kUnicode));

  auto flt = Ref<>::steal(PyFloat_FromDouble(1.5));
  BinaryOpCache float_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, float_cache, flt, flt),
      sameTypes(SpecializedType::kFloat));

  auto list = Ref<>::steal(PyList_New(0));
  BinaryOpCache list_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, list_cache, list, list),
      sameTypes(SpecializedType::kList));

  // bytes has no SpecializedType, so it goes straight to the generic path.
  auto bytes = Ref<>::steal(PyBytes_FromString("x"));
  BinaryOpCache generic_cache{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, generic_cache, bytes, bytes),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheSpecializationFallbackLookup) {
  using SpecializedType = SpecializedType;

  // Compact long first, then non-compact ints: the compact-long guard falls
  // back to the general long SpecializedType rather than all the way to
  // generic.
  auto small = Ref<>::steal(PyLong_FromLong(1));
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  BinaryOpCache compact_to_long{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, compact_to_long, small, small),
      sameTypes(SpecializedType::kCompactLong));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, compact_to_long, big, big),
      sameTypes(SpecializedType::kLong));

  // Int first, then float: the long guard falls back to the generic path.
  auto flt = Ref<>::steal(PyFloat_FromDouble(1.0));
  BinaryOpCache long_to_generic{BinaryOpKind::kAdd};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, long_to_generic, small, small),
      sameTypes(SpecializedType::kCompactLong));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, long_to_generic, flt, flt),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheRejectsUnsupportedOpKind) {
  // Only add, multiply and subtract are currently supported; constructing a
  // cache for any other op kind should throw rather than silently produce a
  // broken cache.
  EXPECT_THROW(BinaryOpCache{BinaryOpKind::kFloorDivide}, std::runtime_error);
}

TEST_F(InlineCacheTest, BinaryOpCacheMultiplySpecializationLookup) {
  using SpecializedType = SpecializedType;

  auto count = Ref<>::steal(PyLong_FromLong(3));

  // Small ints specialize to compact-long multiply.
  auto two = Ref<>::steal(PyLong_FromLong(2));
  BinaryOpCache compact{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, compact, two, count),
      sameTypes(SpecializedType::kCompactLong));
  // Non-compact ints fall back to the general long-multiply specialization.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, compact, big, big),
      sameTypes(SpecializedType::kLong));

  // A general (long, long) multiply that never saw compact operands.
  BinaryOpCache long_long{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, long_long, big, big),
      sameTypes(SpecializedType::kLong));

  auto flt = Ref<>::steal(PyFloat_FromDouble(1.5));
  BinaryOpCache float_float{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, float_float, flt, flt),
      sameTypes(SpecializedType::kFloat));

  // The (sequence, long) specializations have distinct lhs/rhs/return types.
  auto list = Ref<>::steal(PyList_New(0));
  BinaryOpCache list_long{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, list_long, list, count),
      types(
          SpecializedType::kList,
          SpecializedType::kLong,
          SpecializedType::kList));

  auto str = Ref<>::steal(PyUnicode_FromString("ab"));
  BinaryOpCache str_long{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, str_long, str, count),
      types(
          SpecializedType::kUnicode,
          SpecializedType::kLong,
          SpecializedType::kUnicode));

  auto tuple = Ref<>::steal(PyTuple_New(0));
  BinaryOpCache tuple_long{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, tuple_long, tuple, count),
      types(
          SpecializedType::kTuple,
          SpecializedType::kLong,
          SpecializedType::kTuple));

  auto cplx = Ref<>::steal(PyComplex_FromDoubles(1.0, 2.0));
  BinaryOpCache complex_long{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, complex_long, cplx, count),
      types(
          SpecializedType::kComplex,
          SpecializedType::kLong,
          SpecializedType::kComplex));

  // A multiply combination with no specialization falls back to generic.
  auto bytes = Ref<>::steal(PyBytes_FromString("x"));
  BinaryOpCache generic{BinaryOpKind::kMultiply};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::multiply, generic, bytes, count),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheMultiplyComputesCorrectly) {
  auto count = Ref<>::steal(PyLong_FromLong(3));

  // long * long.
  BinaryOpCache long_cache{BinaryOpKind::kMultiply};
  auto four = Ref<>::steal(PyLong_FromLong(4));
  auto product =
      Ref<>::steal(BinaryOpCache::multiply(four, count, &long_cache));
  ASSERT_NE(product.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(product), 12);

  // list * int repeats the list.
  BinaryOpCache list_cache{BinaryOpKind::kMultiply};
  auto list = Ref<>::steal(PyList_New(0));
  PyList_Append(list, four);
  auto repeated =
      Ref<>::steal(BinaryOpCache::multiply(list, count, &list_cache));
  ASSERT_NE(repeated.get(), nullptr);
  EXPECT_EQ(PyList_Size(repeated), 3);

  // str * int repeats the string.
  BinaryOpCache str_cache{BinaryOpKind::kMultiply};
  auto str = Ref<>::steal(PyUnicode_FromString("ab"));
  auto repeated_str =
      Ref<>::steal(BinaryOpCache::multiply(str, count, &str_cache));
  ASSERT_NE(repeated_str.get(), nullptr);
  auto expected_str = Ref<>::steal(PyUnicode_FromString("ababab"));
  EXPECT_EQ(PyObject_RichCompareBool(repeated_str, expected_str, Py_EQ), 1);
}

TEST_F(InlineCacheTest, BinaryOpCacheCompactAddDeoptsOnNonCompactResult) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kAdd};

  // Both operands are compact (< 2^30) but their sum (2^30) is not, so the
  // compact fast path computes the correct result and steps down one level: to
  // compact/compact/long, which keeps the compact-args fast path but no longer
  // checks the result.
  auto compact = Ref<>::steal(PyLong_FromLong(1L << 29));
  auto result = Ref<>::steal(BinaryOpCache::add(compact, compact, &cache));
  ASSERT_NE(result.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 1L << 30);
  EXPECT_EQ(
      cache.specializedTypes(),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));
}

TEST_F(InlineCacheTest, BinaryOpCacheCompactMultiplyDeoptsOnNonCompactResult) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kMultiply};

  // 2^20 is compact but 2^20 * 2^20 == 2^40 is not, so the compact fast path
  // steps down one level to compact/compact/long.
  auto compact = Ref<>::steal(PyLong_FromLong(1L << 20));
  auto result = Ref<>::steal(BinaryOpCache::multiply(compact, compact, &cache));
  ASSERT_NE(result.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 1L << 40);
  EXPECT_EQ(
      cache.specializedTypes(),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));
}

TEST_F(InlineCacheTest, BinaryOpCacheCompactAddStepsDownChain) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kAdd};

  // Compact args with a compact result -> compact/compact/compact.
  auto small = Ref<>::steal(PyLong_FromLong(1));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, cache, small, small),
      sameTypes(SpecializedType::kCompactLong));

  // Compact args with a non-compact result -> steps down to
  // compact/compact/long (still uses the compact-args fast path).
  auto half = Ref<>::steal(PyLong_FromLong(1L << 29));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, cache, half, half),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));

  // Compact args, non-compact result again -> stays put; compact/compact/long
  // no longer checks the result.
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, cache, half, half),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));

  // Non-compact args -> steps down to long/long/long.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::add, cache, big, big),
      sameTypes(SpecializedType::kLong));
}

TEST_F(InlineCacheTest, BinaryOpCacheSubtractSpecializationLookup) {
  using SpecializedType = SpecializedType;

  // Small ints fit in a single digit -> compact-long SpecializedType.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  auto one = Ref<>::steal(PyLong_FromLong(1));
  BinaryOpCache compact{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, compact, seven, one),
      sameTypes(SpecializedType::kCompactLong));

  // Large ints span multiple digits -> general long SpecializedType.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  BinaryOpCache long_long{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, long_long, big, big),
      sameTypes(SpecializedType::kLong));

  auto flt = Ref<>::steal(PyFloat_FromDouble(1.5));
  BinaryOpCache float_float{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, float_float, flt, flt),
      sameTypes(SpecializedType::kFloat));

  auto cplx = Ref<>::steal(PyComplex_FromDoubles(3.0, 4.0));
  BinaryOpCache complex_complex{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, complex_complex, cplx, cplx),
      sameTypes(SpecializedType::kComplex));

  auto set = makeSet({1, 2, 3});
  ASSERT_NE(set.get(), nullptr);
  BinaryOpCache set_set{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, set_set, set, set),
      sameTypes(SpecializedType::kSet));

  // frozenset supports '-' but is not an exact set, so it misses every
  // specialization and lands on the generic path rather than the set fast path.
  auto frozen = Ref<>::steal(PyFrozenSet_New(nullptr));
  ASSERT_NE(frozen.get(), nullptr);
  BinaryOpCache generic{BinaryOpKind::kSubtract};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, generic, frozen, frozen),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheSubtractComputesCorrectly) {
  auto ten = Ref<>::steal(PyLong_FromLong(10));
  auto four = Ref<>::steal(PyLong_FromLong(4));

  // compact int - compact int, including a negative result.
  BinaryOpCache compact{BinaryOpKind::kSubtract};
  auto diff = Ref<>::steal(BinaryOpCache::subtract(ten, four, &compact));
  ASSERT_NE(diff.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(diff), 6);
  auto negative = Ref<>::steal(BinaryOpCache::subtract(four, ten, &compact));
  ASSERT_NE(negative.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(negative), -6);

  // long - long.
  BinaryOpCache long_long{BinaryOpKind::kSubtract};
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto big_diff = Ref<>::steal(BinaryOpCache::subtract(big, one, &long_long));
  ASSERT_NE(big_diff.get(), nullptr);
  EXPECT_EQ(PyLong_AsLongLong(big_diff), (1LL << 60) - 1);

  // float - float.
  BinaryOpCache float_float{BinaryOpKind::kSubtract};
  auto a = Ref<>::steal(PyFloat_FromDouble(1.5));
  auto b = Ref<>::steal(PyFloat_FromDouble(0.25));
  auto float_diff = Ref<>::steal(BinaryOpCache::subtract(a, b, &float_float));
  ASSERT_NE(float_diff.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(float_diff));
  EXPECT_EQ(PyFloat_AsDouble(float_diff), 1.25);

  // complex - complex.
  BinaryOpCache complex_complex{BinaryOpKind::kSubtract};
  auto lhs_c = Ref<>::steal(PyComplex_FromDoubles(3.0, 4.0));
  auto rhs_c = Ref<>::steal(PyComplex_FromDoubles(1.0, 2.0));
  auto complex_diff =
      Ref<>::steal(BinaryOpCache::subtract(lhs_c, rhs_c, &complex_complex));
  ASSERT_NE(complex_diff.get(), nullptr);
  EXPECT_EQ(PyComplex_RealAsDouble(complex_diff), 2.0);
  EXPECT_EQ(PyComplex_ImagAsDouble(complex_diff), 2.0);

  // set - set is difference.
  BinaryOpCache set_set{BinaryOpKind::kSubtract};
  auto lhs_s = makeSet({1, 2, 3});
  auto rhs_s = makeSet({2});
  auto expected_s = makeSet({1, 3});
  ASSERT_NE(lhs_s.get(), nullptr);
  ASSERT_NE(rhs_s.get(), nullptr);
  ASSERT_NE(expected_s.get(), nullptr);
  auto set_diff = Ref<>::steal(BinaryOpCache::subtract(lhs_s, rhs_s, &set_set));
  ASSERT_NE(set_diff.get(), nullptr);
  EXPECT_EQ(PyObject_RichCompareBool(set_diff, expected_s, Py_EQ), 1);
}

TEST_F(InlineCacheTest, BinaryOpCacheCompactSubtractDeoptsOnNonCompactResult) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kSubtract};

  // Both operands are compact (|v| < 2^30) but their difference (-2^30) is
  // not, so the compact fast path computes the correct result and steps down
  // one level to compact/compact/long.
  auto neg = Ref<>::steal(PyLong_FromLong(-(1L << 29)));
  auto pos = Ref<>::steal(PyLong_FromLong(1L << 29));
  auto result = Ref<>::steal(BinaryOpCache::subtract(neg, pos, &cache));
  ASSERT_NE(result.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(result), -(1L << 30));
  EXPECT_EQ(
      cache.specializedTypes(),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));
}

TEST_F(InlineCacheTest, BinaryOpCacheSubtractStepsDownChain) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kSubtract};

  // Compact args with a compact result -> compact/compact/compact.
  auto three = Ref<>::steal(PyLong_FromLong(3));
  auto one = Ref<>::steal(PyLong_FromLong(1));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, cache, three, one),
      sameTypes(SpecializedType::kCompactLong));

  // Compact args with a non-compact result -> compact/compact/long.
  auto neg = Ref<>::steal(PyLong_FromLong(-(1L << 29)));
  auto pos = Ref<>::steal(PyLong_FromLong(1L << 29));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, cache, neg, pos),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kLong));

  // Non-compact args -> long/long/long.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, cache, big, big),
      sameTypes(SpecializedType::kLong));

  // An operand type with no subtract specialization -> generic, permanently.
  auto frozen = Ref<>::steal(PyFrozenSet_New(nullptr));
  ASSERT_NE(frozen.get(), nullptr);
  EXPECT_EQ(
      specializeWith(BinaryOpCache::subtract, cache, frozen, frozen),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheSubtractSetThenIntRaisesTypeError) {
  BinaryOpCache cache{BinaryOpKind::kSubtract};

  auto lhs = makeSet({1, 2});
  auto rhs = makeSet({2});
  auto expected = makeSet({1});
  ASSERT_NE(lhs.get(), nullptr);
  ASSERT_NE(rhs.get(), nullptr);
  ASSERT_NE(expected.get(), nullptr);

  auto diff = Ref<>::steal(BinaryOpCache::subtract(lhs, rhs, &cache));
  ASSERT_NE(diff.get(), nullptr);
  EXPECT_EQ(PyObject_RichCompareBool(diff, expected, Py_EQ), 1);

  // set - int is a TypeError: the set guard fails, the cache steps down to the
  // generic path and PyNumber_Subtract raises.
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto err = Ref<>::steal(BinaryOpCache::subtract(lhs, one, &cache));
  EXPECT_EQ(err.get(), nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
  PyErr_Clear();

  // The state machine survives the raise: a valid difference still works.
  auto again = Ref<>::steal(BinaryOpCache::subtract(lhs, rhs, &cache));
  ASSERT_NE(again.get(), nullptr);
  EXPECT_EQ(PyObject_RichCompareBool(again, expected, Py_EQ), 1);
}

TEST_F(InlineCacheTest, BinaryOpCacheTrueDivideSpecializationLookup) {
  using SpecializedType = SpecializedType;

  // float / float is the row that matters most: CPython has no
  // BINARY_OP_TRUE_DIVIDE specialization, so these sites always reach the
  // cache.
  auto flt = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto four_f = Ref<>::steal(PyFloat_FromDouble(4.0));
  BinaryOpCache float_float{BinaryOpKind::kTrueDivide};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, float_float, flt, four_f),
      sameTypes(SpecializedType::kFloat));

  // int / int yields a float, so the return type differs from the operands.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  BinaryOpCache compact{BinaryOpKind::kTrueDivide};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, compact, seven, two),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kFloat));

  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  auto half_big = Ref<>::steal(PyLong_FromLong(1L << 59));
  BinaryOpCache long_long{BinaryOpKind::kTrueDivide};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, long_long, big, half_big),
      types(
          SpecializedType::kLong,
          SpecializedType::kLong,
          SpecializedType::kFloat));

  auto cplx = Ref<>::steal(PyComplex_FromDoubles(4.0, 0.0));
  auto cplx_two = Ref<>::steal(PyComplex_FromDoubles(2.0, 0.0));
  BinaryOpCache complex_complex{BinaryOpKind::kTrueDivide};
  EXPECT_EQ(
      specializeWith(
          BinaryOpCache::trueDivide, complex_complex, cplx, cplx_two),
      sameTypes(SpecializedType::kComplex));

  // bool divides fine but is not an exact int, so it misses every
  // specialization and lands on the generic path.
  BinaryOpCache generic{BinaryOpKind::kTrueDivide};
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, generic, Py_True, Py_True),
      sameTypes(SpecializedType::kGeneric));
}

TEST_F(InlineCacheTest, BinaryOpCacheTrueDivideComputesCorrectly) {
  // int / int returns a float, not an int.
  BinaryOpCache compact{BinaryOpKind::kTrueDivide};
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto quotient = Ref<>::steal(BinaryOpCache::trueDivide(seven, two, &compact));
  ASSERT_NE(quotient.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(quotient));
  EXPECT_EQ(PyFloat_AsDouble(quotient), 3.5);

  // Negative operands keep the compact fast path.
  auto minus_seven = Ref<>::steal(PyLong_FromLong(-7));
  auto negative =
      Ref<>::steal(BinaryOpCache::trueDivide(minus_seven, two, &compact));
  ASSERT_NE(negative.get(), nullptr);
  EXPECT_EQ(PyFloat_AsDouble(negative), -3.5);

  // long / long.
  BinaryOpCache long_long{BinaryOpKind::kTrueDivide};
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  auto half_big = Ref<>::steal(PyLong_FromLong(1L << 59));
  auto big_quotient =
      Ref<>::steal(BinaryOpCache::trueDivide(big, half_big, &long_long));
  ASSERT_NE(big_quotient.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(big_quotient));
  EXPECT_EQ(PyFloat_AsDouble(big_quotient), 2.0);

  // float / float.
  BinaryOpCache float_float{BinaryOpKind::kTrueDivide};
  auto one_f = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto four_f = Ref<>::steal(PyFloat_FromDouble(4.0));
  auto float_quotient =
      Ref<>::steal(BinaryOpCache::trueDivide(one_f, four_f, &float_float));
  ASSERT_NE(float_quotient.get(), nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(float_quotient));
  EXPECT_EQ(PyFloat_AsDouble(float_quotient), 0.25);

  // complex / complex.
  BinaryOpCache complex_complex{BinaryOpKind::kTrueDivide};
  auto cplx = Ref<>::steal(PyComplex_FromDoubles(4.0, 2.0));
  auto cplx_two = Ref<>::steal(PyComplex_FromDoubles(2.0, 0.0));
  auto complex_quotient =
      Ref<>::steal(BinaryOpCache::trueDivide(cplx, cplx_two, &complex_complex));
  ASSERT_NE(complex_quotient.get(), nullptr);
  EXPECT_EQ(PyComplex_RealAsDouble(complex_quotient), 2.0);
  EXPECT_EQ(PyComplex_ImagAsDouble(complex_quotient), 1.0);
}

TEST_F(InlineCacheTest, BinaryOpCacheTrueDivideMatchesPyNumber) {
  // The compact-int row divides two doubles inline instead of calling
  // long_true_divide, so every interesting compact case is checked against the
  // real thing.  1/3 and 2/3 exercise rounding; 0 / -3 exercises the sign of a
  // zero result, which a naive implementation gets wrong.
  auto zero = Ref<>::steal(PyLong_FromLong(0));
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto three = Ref<>::steal(PyLong_FromLong(3));
  auto minus_three = Ref<>::steal(PyLong_FromLong(-3));
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  auto max_compact = Ref<>::steal(PyLong_FromLong((1L << 30) - 1));

  expectTrueDivideMatchesPyNumber(one, three);
  expectTrueDivideMatchesPyNumber(two, three);
  expectTrueDivideMatchesPyNumber(seven, two);
  expectTrueDivideMatchesPyNumber(zero, minus_three);
  expectTrueDivideMatchesPyNumber(zero, three);
  expectTrueDivideMatchesPyNumber(minus_three, seven);
  expectTrueDivideMatchesPyNumber(max_compact, three);
  expectTrueDivideMatchesPyNumber(one, max_compact);

  // Non-compact operands go through long_true_divide on both sides, but check
  // a few anyway: these are the values where a double-based shortcut would
  // lose the correctly-rounded result.
  auto big = Ref<>::steal(PyLong_FromLong((1L << 60) + 1));
  auto big_minus_one = Ref<>::steal(PyLong_FromLong((1L << 60) - 1));
  expectTrueDivideMatchesPyNumber(big, three);
  expectTrueDivideMatchesPyNumber(big, big_minus_one);

  // The float row divides inline too, so pin it the same way -- including the
  // non-finite inputs where a shortcut could diverge from float_div.
  auto one_f = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto three_f = Ref<>::steal(PyFloat_FromDouble(3.0));
  auto minus_two_f = Ref<>::steal(PyFloat_FromDouble(-2.0));
  auto zero_f = Ref<>::steal(PyFloat_FromDouble(0.0));
  auto inf_f = Ref<>::steal(PyFloat_FromDouble(HUGE_VAL));
  auto nan_f = Ref<>::steal(PyFloat_FromDouble(std::nan("")));
  auto tiny_f = Ref<>::steal(PyFloat_FromDouble(5e-324));
  auto huge_f = Ref<>::steal(PyFloat_FromDouble(1.7976931348623157e308));

  expectTrueDivideMatchesPyNumber(one_f, three_f);
  expectTrueDivideMatchesPyNumber(one_f, minus_two_f);
  // 0.0 / -2.0 is -0.0; the signbit check is what catches getting this wrong.
  expectTrueDivideMatchesPyNumber(zero_f, minus_two_f);
  expectTrueDivideMatchesPyNumber(inf_f, three_f);
  expectTrueDivideMatchesPyNumber(three_f, inf_f);
  expectTrueDivideMatchesPyNumber(nan_f, three_f);
  expectTrueDivideMatchesPyNumber(three_f, nan_f);
  // Overflow to inf and underflow to zero must match float_div, not raise.
  expectTrueDivideMatchesPyNumber(huge_f, tiny_f);
  expectTrueDivideMatchesPyNumber(tiny_f, huge_f);
}

TEST_F(InlineCacheTest, BinaryOpCacheTrueDivideByZeroRaises) {
  using SpecializedType = SpecializedType;

  // Each specialization must raise ZeroDivisionError rather than producing an
  // inf/nan, and must not let the raise disturb the specialization it settled
  // on.
  {
    BinaryOpCache cache{BinaryOpKind::kTrueDivide};
    auto six = Ref<>::steal(PyLong_FromLong(6));
    auto three = Ref<>::steal(PyLong_FromLong(3));
    auto zero = Ref<>::steal(PyLong_FromLong(0));
    Ref<>::steal(BinaryOpCache::trueDivide(six, three, &cache));
    BinaryOpCache::BinarySpecialization before = cache.specializedTypes();

    auto err = Ref<>::steal(BinaryOpCache::trueDivide(six, zero, &cache));
    EXPECT_EQ(err.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();
    EXPECT_EQ(cache.specializedTypes(), before);

    auto ok = Ref<>::steal(BinaryOpCache::trueDivide(six, three, &cache));
    ASSERT_NE(ok.get(), nullptr);
    EXPECT_EQ(PyFloat_AsDouble(ok), 2.0);
  }

  // Non-compact int / zero.
  {
    BinaryOpCache cache{BinaryOpKind::kTrueDivide};
    auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
    auto zero = Ref<>::steal(PyLong_FromLong(0));
    Ref<>::steal(BinaryOpCache::trueDivide(big, big, &cache));
    EXPECT_EQ(
        cache.specializedTypes(),
        types(
            SpecializedType::kLong,
            SpecializedType::kLong,
            SpecializedType::kFloat));

    auto err = Ref<>::steal(BinaryOpCache::trueDivide(big, zero, &cache));
    EXPECT_EQ(err.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();
  }

  // float / 0.0.
  {
    BinaryOpCache cache{BinaryOpKind::kTrueDivide};
    auto one_f = Ref<>::steal(PyFloat_FromDouble(1.0));
    auto zero_f = Ref<>::steal(PyFloat_FromDouble(0.0));
    Ref<>::steal(BinaryOpCache::trueDivide(one_f, one_f, &cache));

    auto err = Ref<>::steal(BinaryOpCache::trueDivide(one_f, zero_f, &cache));
    EXPECT_EQ(err.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();
  }

  // complex / 0j.
  {
    BinaryOpCache cache{BinaryOpKind::kTrueDivide};
    auto cplx = Ref<>::steal(PyComplex_FromDoubles(1.0, 1.0));
    auto zero_c = Ref<>::steal(PyComplex_FromDoubles(0.0, 0.0));
    Ref<>::steal(BinaryOpCache::trueDivide(cplx, cplx, &cache));

    auto err = Ref<>::steal(BinaryOpCache::trueDivide(cplx, zero_c, &cache));
    EXPECT_EQ(err.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();
  }
}

TEST_F(InlineCacheTest, BinaryOpCacheTrueDivideStepsDownChain) {
  using SpecializedType = SpecializedType;
  BinaryOpCache cache{BinaryOpKind::kTrueDivide};

  // Compact ints -> compact/compact/float.  There is no result check here (the
  // return type is Float, which the op always produces), so unlike add and
  // subtract there is no compact -> long step driven by the result.
  auto six = Ref<>::steal(PyLong_FromLong(6));
  auto three = Ref<>::steal(PyLong_FromLong(3));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, cache, six, three),
      types(
          SpecializedType::kCompactLong,
          SpecializedType::kCompactLong,
          SpecializedType::kFloat));

  // Non-compact args -> long/long/float.
  auto big = Ref<>::steal(PyLong_FromLong(1L << 60));
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, cache, big, big),
      types(
          SpecializedType::kLong,
          SpecializedType::kLong,
          SpecializedType::kFloat));

  // An operand type with no true-divide specialization -> generic, permanently.
  EXPECT_EQ(
      specializeWith(BinaryOpCache::trueDivide, cache, Py_True, Py_True),
      sameTypes(SpecializedType::kGeneric));
}

// Load/StoreAttrCache dispatch through a function pointer held in the cache
// (CINDERX_IC_USE_TARGET_PROMOTION). The tests below drive that pointer the
// same way generated code does: load target_ via targetAddr() and call it with
// the cache as the last argument.

namespace {

// Attribute caches end in a flexible entries_[0] array, so they cannot be
// stack- or new-allocated directly; the allocation has to be sized by
// AttributeCacheSizeTrait. Mirrors what SlabArena does.
template <typename T>
class CacheStorage {
 public:
  CacheStorage() : raw_{::operator new(AttributeCacheSizeTrait::size())} {
    cache_ = new (raw_) T();
  }
  ~CacheStorage() {
    cache_->~T();
    ::operator delete(raw_);
  }
  CacheStorage(const CacheStorage&) = delete;
  CacheStorage& operator=(const CacheStorage&) = delete;

  T* operator->() {
    return cache_;
  }
  T* get() {
    return cache_;
  }

 private:
  void* raw_;
  T* cache_;
};

// Runs `src` and returns its locals, or nullptr on failure.
Ref<> runToLocals(RuntimeTest* test, const char* src) {
  Ref<PyObject> globals(test->MakeGlobals());
  if (globals == nullptr) {
    return nullptr;
  }
  auto locals = Ref<>::steal(PyDict_New());
  if (locals == nullptr) {
    return nullptr;
  }
  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  if (st == nullptr) {
    PyErr_Print();
    return nullptr;
  }
  return locals;
}

Ref<> callLoad(LoadAttrCache* cache, BorrowedRef<> obj, BorrowedRef<> name) {
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  return Ref<>::steal((*cache->targetAddr())(obj, name, cache));
#else
  return Ref<>::steal(LoadAttrCache::invoke(obj, name, cache));
#endif
}

int callStore(
    StoreAttrCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name,
    BorrowedRef<> value) {
#ifdef CINDERX_IC_USE_TARGET_PROMOTION
  return (*cache->targetAddr())(obj, name, value, cache);
#else
  return StoreAttrCache::invoke(obj, name, value, cache);
#endif
}

long asLong(BorrowedRef<> obj) {
  return PyLong_AsLong(obj);
}

constexpr const char* kTwoTypes = R"(
class C:
  def __init__(self):
    self.x = 5

class D:
  def __init__(self):
    self.x = 11

c = C()
d = D()
)";

} // namespace

#ifdef CINDERX_IC_USE_TARGET_PROMOTION
TEST_F(InlineCacheTest, AttrCacheStartsEmptyWhateverItsSize) {
  // Every cache begins on the "nothing cached yet" entry point regardless of
  // attr_cache_size. Size no longer decides the dispatch slot up front; the
  // number of populated entries does, and that is re-evaluated as the cache
  // fills.
  for (uint32_t size : {1u, 4u}) {
    getMutableConfig().attr_cache_size = size;
    CacheStorage<LoadAttrCache> load;
    CacheStorage<StoreAttrCache> store;
    EXPECT_EQ(*load->targetAddr(), LoadAttrCache::invokeEmpty)
        << "attr_cache_size = " << size;
    EXPECT_EQ(*store->targetAddr(), StoreAttrCache::invokeEmpty)
        << "attr_cache_size = " << size;
  }
}

TEST_F(InlineCacheTest, MultiEntryAttrCacheDemotesToScanOnSecondType) {
  // A multi-entry cache still gets the monomorphic fast path while it has only
  // seen one type, and falls back to the scan once a second type forces a
  // second entry.
  getMutableConfig().attr_cache_size = 4;
  auto locals = runToLocals(this, kTwoTypes);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  BorrowedRef<> d = PyDict_GetItemString(locals, "d");
  ASSERT_NE(c.get(), nullptr);
  ASSERT_NE(d.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(*cache->targetAddr(), LoadAttrCache::invokeEmpty);

  auto res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);
  LoadAttrTarget specialized = *cache->targetAddr();
  EXPECT_NE(specialized, LoadAttrCache::invokeEmpty)
      << "One cached type should promote to a specialized entry point";
  EXPECT_NE(specialized, LoadAttrCache::invoke)
      << "One cached type should not need the scan";

  // A second receiver type cannot be served from slot 0, so the cache moves to
  // the two-entry unrolled entry point rather than all the way to the scan.
  res = callLoad(cache.get(), d, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 11);
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeUnrolled<2>)
      << "A second cached type should demote to the two-entry entry point";

  // Both types keep working from there.
  res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);
  res = callLoad(cache.get(), d, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 11);
}

TEST_F(InlineCacheTest, AttrCachePacksEntriesAfterInvalidation) {
  // Invalidating an entry in the middle has to close the hole, because the
  // unrolled entry points read entries_[0 .. N-1] straight through and would
  // otherwise walk past a live entry that had been stranded behind a gap.
  getMutableConfig().attr_cache_size = 4;
  auto locals = runToLocals(this, R"(
class A:
  def __init__(self):
    self.x = 1

class B:
  def __init__(self):
    self.x = 2

class C:
  def __init__(self):
    self.x = 3

a = A()
b = B()
c = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> a = PyDict_GetItemString(locals, "a");
  BorrowedRef<> b = PyDict_GetItemString(locals, "b");
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  BorrowedRef<PyTypeObject> type_b = PyDict_GetItemString(locals, "B");
  ASSERT_NE(a.get(), nullptr);
  ASSERT_NE(b.get(), nullptr);
  ASSERT_NE(c.get(), nullptr);
  ASSERT_NE(type_b.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  // Fill three entries, in order, so B lands in the middle.
  CacheStorage<LoadAttrCache> cache;
  for (auto obj : {a, b, c}) {
    auto res = callLoad(cache.get(), obj, name);
    ASSERT_NE(res.get(), nullptr);
  }
  ASSERT_EQ(*cache->targetAddr(), LoadAttrCache::invokeUnrolled<3>)
      << "Three cached types should use the three-entry entry point";

  // Knock out the middle one.
  auto st = Ref<>::steal(PyRun_String(
      "B.x = property(lambda self: 99)\n", Py_file_input, locals, locals));
  ASSERT_NE(st.get(), nullptr);
  notifyICsTypeChanged(type_b);

  // A and C must both still be served correctly; if packing left a hole where
  // B was, C would be stranded at index 2 behind it.
  auto res = callLoad(cache.get(), a, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 1);
  res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 3);
  // And B now resolves through its new descriptor.
  res = callLoad(cache.get(), b, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 99);
}

TEST_F(InlineCacheTest, AttrCacheSpecializesTargetOnFirstFill) {
  // Filling the single entry re-points the dispatch slot at a Kind-specialized
  // entry point, so the steady-state call never runs the Kind switch. Which
  // specialization is picked depends on the receiver layout and Python
  // version, so assert on the transition rather than the identity.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, kTwoTypes);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  ASSERT_NE(c.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(*cache->targetAddr(), LoadAttrCache::invokeEmpty);

  auto res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);

  LoadAttrTarget specialized = *cache->targetAddr();
  EXPECT_NE(specialized, LoadAttrCache::invokeEmpty)
      << "A filled cache should have left the empty entry point";
  EXPECT_NE(specialized, LoadAttrCache::invoke)
      << "A single-entry cache should never fall back to the scan";

  // A steady-state hit must not disturb the specialization.
  res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);
  EXPECT_EQ(*cache->targetAddr(), specialized);
}

TEST_F(InlineCacheTest, AttrCacheRespecializesAfterInvalidation) {
  // An invalidated entry is reset to a null type, which no live receiver can
  // match, so it fails the specialized guard and routes to the miss handler,
  // which re-specializes against whatever the refill produced.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  def __init__(self):
    self.x = 5

obj = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  BorrowedRef<PyTypeObject> type = PyDict_GetItemString(locals, "C");
  ASSERT_NE(obj.get(), nullptr);
  ASSERT_NE(type.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  auto res = callLoad(cache.get(), obj, name);
  ASSERT_NE(res.get(), nullptr);
  LoadAttrTarget instance_target = *cache->targetAddr();
  ASSERT_NE(instance_target, LoadAttrCache::invokeEmpty);

  // Shadow the instance attribute with a data descriptor. That is a different
  // AttributeMutator::Kind, so the cache must land on a different
  // specialization, not just a different cached type.
  auto st = Ref<>::steal(PyRun_String(
      "C.x = property(lambda self: 99)\n", Py_file_input, locals, locals));
  ASSERT_NE(st.get(), nullptr);
  notifyICsTypeChanged(type);

  res = callLoad(cache.get(), obj, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 99);
  EXPECT_NE(*cache->targetAddr(), instance_target)
      << "A refill with a different Kind must install a different target";

  // And the new specialization must itself be stable and correct.
  for (int i = 0; i < 2; i++) {
    res = callLoad(cache.get(), obj, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 99) << "iteration " << i;
  }
}

namespace {

constexpr const char* kTwoModules = R"(
import types

a = types.ModuleType("a")
a.x = 5

b = types.ModuleType("b")
b.x = 11
)";

// Repeat a load enough times to get past the two warm-up misses -- the first
// installs the kModule target, the second populates the mutator -- so that what
// is being measured is the steady-state cached path.
long loadRepeatedly(
    LoadAttrCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  long last = -1;
  for (int i = 0; i < 3; i++) {
    auto res = callLoad(cache, obj, name);
    if (res == nullptr) {
      return -1;
    }
    last = asLong(res);
  }
  return last;
}

} // namespace

TEST_F(InlineCacheTest, AttrCacheSpecializesModuleLoads) {
  Ref<> locals = runToLocals(this, kTwoModules);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> mod = PyDict_GetItemString(locals, "a");
  ASSERT_NE(mod.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  auto res = callLoad(cache.get(), mod, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);

  // A module receiver must land on the module specialization. Before kModule
  // existed, fill() rejected modules outright and the cache stayed empty.
  // retarget() monomorphises the cache to modules and installs invokeModule
  // rather than the generic single-entry specialization.
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeModule)
      << "A module load should install the invokeModule target";

  EXPECT_EQ(loadRepeatedly(cache.get(), mod, name), 5)
      << "The cached module read should be stable across repeats";
}

TEST_F(InlineCacheTest, ModuleAttrCacheSeesDictMutation) {
  Ref<> locals = runToLocals(this, kTwoModules);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> mod = PyDict_GetItemString(locals, "a");
  ASSERT_NE(mod.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), mod, name), 5);

  // Rebinding the attribute bumps the module dict's version tag, which is the
  // only thing standing between the cache and a stale value -- no type changes
  // here, so the type watcher never fires.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttr(mod, name, seven), 0);

  EXPECT_EQ(loadRepeatedly(cache.get(), mod, name), 7)
      << "A module dict mutation must invalidate the cached value";
}

TEST_F(InlineCacheTest, ModuleAttrCacheChecksModuleIdentity) {
  Ref<> locals = runToLocals(this, kTwoModules);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> mod_a = PyDict_GetItemString(locals, "a");
  BorrowedRef<> mod_b = PyDict_GetItemString(locals, "b");
  ASSERT_NE(mod_a.get(), nullptr);
  ASSERT_NE(mod_b.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), mod_a, name), 5);

  // Both modules share a type, so the entry's type guard passes for either one.
  // Only the identity check inside ModuleMutator stops b from being handed a's
  // cached value.
  EXPECT_EQ(loadRepeatedly(cache.get(), mod_b, name), 11)
      << "A different module must not read the first module's cached value";

  // ...and going back to a must not read b's value either.
  EXPECT_EQ(loadRepeatedly(cache.get(), mod_a, name), 5)
      << "Alternating modules must each read their own value";
}

namespace {

constexpr const char* kTwoClasses = R"(
class A:
  x = 5

class B:
  x = 11
)";

} // namespace

TEST_F(InlineCacheTest, AttrCacheSpecializesTypeLoads) {
  Ref<> locals = runToLocals(this, kTwoClasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "A");
  ASSERT_NE(cls.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  auto res = callLoad(cache.get(), cls, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);

  // A type receiver must land on the type target. Before kType existed fill()
  // rejected type objects, because type_getattro is not
  // PyObject_GenericGetAttr, and the cache stayed empty. retarget()
  // monomorphises the cache to classes and installs invokeType rather than the
  // generic single-entry specialization, which could not read a kType entry.
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeType)
      << "A class-attribute load should install the invokeType target";

  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 5)
      << "The cached class read should be stable across repeats";
}

TEST_F(InlineCacheTest, TypeAttrCacheSeesTypeMutation) {
  Ref<> locals = runToLocals(this, kTwoClasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "A");
  ASSERT_NE(cls.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), cls, name), 5);

  // Rebinding the class attribute runs PyType_Modified, which fires the
  // attribute cache's type watcher for A. The entry is keyed on A itself, so
  // typeChanged finds and resets it -- there is no version tag involved.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttr(cls, name, seven), 0);

  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 7)
      << "A class mutation must invalidate the cached value";
}

TEST_F(InlineCacheTest, TypeAttrCacheSeesBaseClassMutation) {
  Ref<> locals = runToLocals(this, R"(
class Base:
  x = 5

class Derived(Base):
  pass
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> base = PyDict_GetItemString(locals, "Base");
  BorrowedRef<> derived = PyDict_GetItemString(locals, "Derived");
  ASSERT_NE(base.get(), nullptr);
  ASSERT_NE(derived.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  // The value is inherited, so it is found on Base but cached against Derived.
  ASSERT_EQ(loadRepeatedly(cache.get(), derived, name), 5);

  // Mutating Base must invalidate an entry keyed on Derived. PyType_Modified
  // recurses into subclasses and fires the watcher once per visited type, so
  // watching only the class that was read is enough to catch a change to
  // anything it inherits from. That recursion is the whole reason this kind can
  // get away with watching a single type.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttr(base, name, seven), 0);

  EXPECT_EQ(loadRepeatedly(cache.get(), derived, name), 7)
      << "A base class mutation must invalidate a value cached on the subclass";
}

TEST_F(InlineCacheTest, TypeAttrCacheChecksClassIdentity) {
  Ref<> locals = runToLocals(this, kTwoClasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls_a = PyDict_GetItemString(locals, "A");
  BorrowedRef<> cls_b = PyDict_GetItemString(locals, "B");
  ASSERT_NE(cls_a.get(), nullptr);
  ASSERT_NE(cls_b.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), cls_a, name), 5);

  // invokeType matches entry.type() against the receiver itself, so A's entry
  // simply does not match B; B gets a second entry of its own rather than A's
  // value.
  EXPECT_EQ(loadRepeatedly(cache.get(), cls_b, name), 11)
      << "A different class must not read the first class's cached value";

  EXPECT_EQ(loadRepeatedly(cache.get(), cls_a, name), 5)
      << "Alternating classes must each read their own value";
}

TEST_F(InlineCacheTest, TypeAttrCacheRejectsInstanceOfCachedClass) {
  // The hazard this kind is built around: the entry holds A in type_, so an
  // *instance* of A is exactly what the generic scan's entry.type() ==
  // Py_TYPE(obj) test would match. invokeType compares against the receiver
  // instead, so the instance must miss and get its own attribute rather than
  // the class's.
  Ref<> locals = runToLocals(this, R"(
class A:
  x = 5

a = A()
a.x = 42
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "A");
  BorrowedRef<> inst = PyDict_GetItemString(locals, "a");
  ASSERT_NE(cls.get(), nullptr);
  ASSERT_NE(inst.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), cls, name), 5);
  ASSERT_EQ(*cache->targetAddr(), LoadAttrCache::invokeType);

  EXPECT_EQ(loadRepeatedly(cache.get(), inst, name), 42)
      << "An instance of the cached class must not be served the class's "
      << "attribute";

  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 5)
      << "...and the class must still read its own";
}

TEST_F(InlineCacheTest, TypeAttrCacheMonomorphisesOverInstanceEntries) {
  // A site that sees instances first and a class later: retarget must clear the
  // instance entries out before handing the cache to invokeType, which assumes
  // every populated entry is a kType keyed on a class.
  getMutableConfig().attr_cache_size = 4;
  Ref<> locals = runToLocals(this, R"(
class A:
  x = 5

class B:
  x = 11

a = A()
a.x = 1
b = B()
b.x = 2
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "A");
  BorrowedRef<> inst_a = PyDict_GetItemString(locals, "a");
  BorrowedRef<> inst_b = PyDict_GetItemString(locals, "b");
  ASSERT_NE(cls.get(), nullptr);
  ASSERT_NE(inst_a.get(), nullptr);
  ASSERT_NE(inst_b.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), inst_a, name), 1);
  ASSERT_EQ(loadRepeatedly(cache.get(), inst_b, name), 2);
  ASSERT_NE(*cache->targetAddr(), LoadAttrCache::invokeType)
      << "Two instance receivers should still be on a generic target";

  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 5);
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeType)
      << "A class receiver must monomorphise the cache to invokeType";

  // The instances now take the fallback, but must still be correct.
  EXPECT_EQ(loadRepeatedly(cache.get(), inst_a, name), 1);
  EXPECT_EQ(loadRepeatedly(cache.get(), inst_b, name), 2);
  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 5);
}

TEST_F(InlineCacheTest, TypeAttrCacheLeavesDescriptorsUncached) {
  // A classmethod has tp_descr_get and is not a plain function or staticmethod,
  // so lookupTypeAttr must run it on every read rather than caching its result.
  // Getting this wrong would hand back a bound method built against a stale
  // class.
  Ref<> locals = runToLocals(this, R"(
class A:
  @classmethod
  def m(cls):
    return 5
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "A");
  ASSERT_NE(cls.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("m"));

  CacheStorage<LoadAttrCache> cache;
  for (int i = 0; i < 3; i++) {
    auto bound = callLoad(cache.get(), cls, name);
    ASSERT_NE(bound.get(), nullptr) << "iteration " << i;
    auto res = Ref<>::steal(PyObject_CallNoArgs(bound));
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 5) << "iteration " << i;
  }
}

namespace {

// The case kMetaType exists for: a classmethod reading a class variable
// through `cls`. The receiver is a class, the value lives on that class's own
// MRO rather than on the metaclass, and the site is polymorphic because `cls`
// is whichever subclass the call arrived on. `class_var` is the load in f's
// body; the tests below drive it directly.
constexpr const char* kMetaclassSubclasses = R"(
class MetaClass(type):
  pass

class Class(metaclass=MetaClass):
  @classmethod
  def f(cls):
    return cls.class_var

class SubClass1(Class):
  class_var = 42

class SubClass2(Class):
  class_var = 100
)";

} // namespace

TEST_F(InlineCacheTest, MetaTypeAttrCacheSpecializesPolymorphicClassLoads) {
  getMutableConfig().attr_cache_size = 4;
  Ref<> locals = runToLocals(this, kMetaclassSubclasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> sub1 = PyDict_GetItemString(locals, "SubClass1");
  BorrowedRef<> sub2 = PyDict_GetItemString(locals, "SubClass2");
  ASSERT_NE(sub1.get(), nullptr);
  ASSERT_NE(sub2.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("class_var"));

  // kType declines these classes -- their metatype is MetaClass, not `type` --
  // so before kMetaType the site gave up and ran the full lookup forever.
  CacheStorage<LoadAttrCache> cache;
  EXPECT_EQ(loadRepeatedly(cache.get(), sub1, name), 42);
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeMetaType)
      << "A class with a metaclass should install the metaclass-aware target";

  // Entries are keyed on the class, so a second one gets its own rather than
  // the first's value.
  EXPECT_EQ(loadRepeatedly(cache.get(), sub2, name), 100);
  EXPECT_EQ(loadRepeatedly(cache.get(), sub1, name), 42)
      << "Alternating classes must each read their own value";
}

TEST_F(InlineCacheTest, MetaTypeAttrCacheSeesClassMutation) {
  Ref<> locals = runToLocals(this, kMetaclassSubclasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> sub1 = PyDict_GetItemString(locals, "SubClass1");
  ASSERT_NE(sub1.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("class_var"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), sub1, name), 42);

  // Same watcher story as kType: the entry is keyed on SubClass1, so
  // PyType_Modified for it reaches typeChanged.
  auto seven = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttr(sub1, name, seven), 0);

  EXPECT_EQ(loadRepeatedly(cache.get(), sub1, name), 7)
      << "A class mutation must invalidate the cached value";
}

TEST_F(InlineCacheTest, MetaTypeAttrCacheSeesMetaclassMutation) {
  Ref<> locals = runToLocals(this, kMetaclassSubclasses);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> sub1 = PyDict_GetItemString(locals, "SubClass1");
  ASSERT_NE(sub1.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("class_var"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), sub1, name), 42);

  // A data descriptor on the metaclass takes precedence over the class's own
  // MRO, so 42 is no longer the answer. MetaClass is not one of SubClass1's
  // bases, so PyType_Modified for it never reaches a watcher this cache holds;
  // the metatype version tag recorded with the value is the only thing that
  // catches this.
  auto st = Ref<>::steal(PyRun_String(
      "MetaClass.class_var = property(lambda cls: 7)\n",
      Py_file_input,
      locals,
      locals));
  ASSERT_NE(st.get(), nullptr) << "Failed adding the metaclass property";

  EXPECT_EQ(loadRepeatedly(cache.get(), sub1, name), 7)
      << "A metaclass data descriptor must override the cached class value";
}

TEST_F(InlineCacheTest, MetaTypeAttrCacheRejectsInstanceOfCachedClass) {
  // The kType hazard, for the new kind: the entry holds SubClass1 in type_, so
  // an *instance* of SubClass1 is what a Py_TYPE(obj) test would match.
  Ref<> locals = runToLocals(this, R"(
class MetaClass(type):
  pass

class SubClass1(metaclass=MetaClass):
  class_var = 42

inst = SubClass1()
inst.class_var = 1
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> sub1 = PyDict_GetItemString(locals, "SubClass1");
  BorrowedRef<> inst = PyDict_GetItemString(locals, "inst");
  ASSERT_NE(sub1.get(), nullptr);
  ASSERT_NE(inst.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("class_var"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), sub1, name), 42);
  ASSERT_EQ(*cache->targetAddr(), LoadAttrCache::invokeMetaType);

  EXPECT_EQ(loadRepeatedly(cache.get(), inst, name), 1)
      << "An instance of the cached class must not be served the class's "
      << "attribute";
  EXPECT_EQ(loadRepeatedly(cache.get(), sub1, name), 42)
      << "...and the class must still read its own";
}

TEST_F(InlineCacheTest, TypeAttrCacheMixesPlainClassesAndMetaclassInstances) {
  // kType and kMetaType survive each other's monomorphisation and share a
  // slow path, so a site alternating between the two keeps an entry for each
  // rather than evicting and re-filling on every call. The dispatch slot
  // follows the leading entry; the other kind is served by the slow path.
  getMutableConfig().attr_cache_size = 4;
  Ref<> locals = runToLocals(this, R"(
class MetaClass(type):
  pass

class WithMeta(metaclass=MetaClass):
  x = 42

class Plain:
  x = 5
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> with_meta = PyDict_GetItemString(locals, "WithMeta");
  BorrowedRef<> plain = PyDict_GetItemString(locals, "Plain");
  ASSERT_NE(with_meta.get(), nullptr);
  ASSERT_NE(plain.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  ASSERT_EQ(loadRepeatedly(cache.get(), plain, name), 5);
  EXPECT_EQ(loadRepeatedly(cache.get(), with_meta, name), 42);
  EXPECT_EQ(loadRepeatedly(cache.get(), plain, name), 5);
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeType)
      << "The leading entry is a plain class, so its target stays installed";
}

TEST_F(InlineCacheTest, MetaTypeAttrCacheDeclinesInterceptingMetaclass) {
  // A metaclass __getattribute__ builds an answer out of more than the class's
  // MRO, which the cache cannot replicate, so it must not claim an entry.
  Ref<> locals = runToLocals(this, R"(
class MetaClass(type):
  def __getattribute__(cls, name):
    return 7

class C(metaclass=MetaClass):
  x = 5
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> cls = PyDict_GetItemString(locals, "C");
  ASSERT_NE(cls.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  EXPECT_EQ(loadRepeatedly(cache.get(), cls, name), 7);
  EXPECT_EQ(*cache->targetAddr(), LoadAttrCache::invokeEmpty)
      << "A metaclass that intercepts the read must leave the cache empty";
}
#endif

TEST_F(InlineCacheTest, LoadAttrCacheSingleEntryHitsAndMisses) {
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, kTwoTypes);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  BorrowedRef<> d = PyDict_GetItemString(locals, "d");
  ASSERT_NE(c.get(), nullptr);
  ASSERT_NE(d.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;

  // First call fills the single entry; second is a guard hit.
  for (int i = 0; i < 2; i++) {
    auto res = callLoad(cache.get(), c, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 5) << "iteration " << i;
  }

  // A second receiver type cannot fit, so it takes the slow path -- but must
  // still be correct, and must not disturb the cached type.
  auto res = callLoad(cache.get(), d, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 11);

  res = callLoad(cache.get(), c, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5);
}

TEST_F(InlineCacheTest, LoadAttrCacheMultiEntryCachesSeveralTypes) {
  // With room for more than one entry the cache keeps its polymorphic
  // behaviour: both receiver types get cached and both stay correct.
  getMutableConfig().attr_cache_size = 4;
  auto locals = runToLocals(this, kTwoTypes);
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  BorrowedRef<> d = PyDict_GetItemString(locals, "d");
  ASSERT_NE(c.get(), nullptr);
  ASSERT_NE(d.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  for (int i = 0; i < 3; i++) {
    auto res = callLoad(cache.get(), c, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 5) << "iteration " << i;
    res = callLoad(cache.get(), d, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 11) << "iteration " << i;
  }
}

TEST_F(InlineCacheTest, LoadAttrCacheUncacheableTypeStaysCorrect) {
  // A custom __getattribute__ means the IC cannot replicate the lookup, so
  // fill() refuses and every call takes the slow path.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  def __getattribute__(self, name):
    return 7

obj = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  ASSERT_NE(obj.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  for (int i = 0; i < 3; i++) {
    auto res = callLoad(cache.get(), obj, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 7) << "iteration " << i;
  }
}

TEST_F(InlineCacheTest, LoadAttrCacheSeesTypeChangeAfterInvalidation) {
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  def __init__(self):
    self.x = 5

obj = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  BorrowedRef<PyTypeObject> type = PyDict_GetItemString(locals, "C");
  ASSERT_NE(obj.get(), nullptr);
  ASSERT_NE(type.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));

  CacheStorage<LoadAttrCache> cache;
  auto res = callLoad(cache.get(), obj, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 5) << "Instance attribute wins before the property";

  // Shadow the instance attribute with a data descriptor, which takes
  // precedence over the instance dict. A stale cache would keep answering 5.
  auto st = Ref<>::steal(PyRun_String(
      "C.x = property(lambda self: 99)\n", Py_file_input, locals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed adding the property";

  // Invalidate explicitly rather than relying on the type-watcher wiring being
  // installed in this fixture.
  notifyICsTypeChanged(type);

  for (int i = 0; i < 2; i++) {
    res = callLoad(cache.get(), obj, name);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 99)
        << "The invalidated cache must observe the new data descriptor, "
        << "iteration " << i;
  }
}

TEST_F(InlineCacheTest, LoadAttrCacheHandlesGetAttrFallback) {
  // __getattr__ performs another attribute access while the outer lookup is
  // still in flight.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  def __init__(self):
    self.real = 12
  def __getattr__(self, name):
    return self.real + 1

obj = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  ASSERT_NE(obj.get(), nullptr);
  auto missing = Ref<>::steal(PyUnicode_FromString("missing"));

  CacheStorage<LoadAttrCache> cache;
  for (int i = 0; i < 3; i++) {
    auto res = callLoad(cache.get(), obj, missing);
    ASSERT_NE(res.get(), nullptr) << "iteration " << i;
    EXPECT_EQ(asLong(res), 13) << "iteration " << i;
  }
}

TEST_F(InlineCacheTest, LoadAttrCacheDescriptorsFallBackToGetAttr) {
  // A descriptor that raises AttributeError on a type with __getattr__ has to
  // reach it. Each descriptor kind runs that fallback from a different body --
  // under target promotion, from a kind of its own that fill() picks only for a
  // type that has a __getattr__ -- so cover all three, and repeat every load so
  // the answer comes from the Kind-specialized entry point rather than from the
  // slow path that populated it.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
def raising_getter(self):
  raise AttributeError("nope")

class Raiser:
  def __get__(self, obj, objtype=None):
    raise AttributeError("nope")

class DataDescr:
  def __getattr__(self, name):
    return 1

class MemberDescr:
  __slots__ = ("x",)
  def __getattr__(self, name):
    return 2

class NonDataDescr:
  def __getattr__(self, name):
    return 3

# Attached out here because a class body resolves names through globals, and
# the definitions above land in the locals runToLocals passes separately.
DataDescr.x = property(raising_getter)
NonDataDescr.x = Raiser()

data_descr = DataDescr()
member_descr = MemberDescr()
non_data_descr = NonDataDescr()
)");
  ASSERT_NE(locals.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));
  ASSERT_NE(name.get(), nullptr);

  auto checkFallsBack = [&](const char* var, long expected) {
    BorrowedRef<> obj = PyDict_GetItemString(locals, var);
    ASSERT_NE(obj.get(), nullptr) << var;
    CacheStorage<LoadAttrCache> cache;
    for (int i = 0; i < 3; i++) {
      auto res = callLoad(cache.get(), obj, name);
      ASSERT_NE(res.get(), nullptr) << var << " iteration " << i;
      EXPECT_EQ(asLong(res), expected) << var << " iteration " << i;
    }
  };

  checkFallsBack("data_descr", 1);
  checkFallsBack("member_descr", 2);
  checkFallsBack("non_data_descr", 3);
}

TEST_F(InlineCacheTest, LoadAttrCacheMemberDescrRaisesWithoutGetAttr) {
  // The counterpart of the above: with no __getattr__ to fall back to, the
  // descriptor's own AttributeError is the answer. Fill the cache off a slot
  // that is set and only then unset it, so the failure runs through the
  // specialized entry point instead of the slow path.
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  __slots__ = ("x",)

obj = C()
obj.x = 7
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  ASSERT_NE(obj.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));
  ASSERT_NE(name.get(), nullptr);

  CacheStorage<LoadAttrCache> cache;
  auto res = callLoad(cache.get(), obj, name);
  ASSERT_NE(res.get(), nullptr);
  EXPECT_EQ(asLong(res), 7);

  ASSERT_EQ(PyObject_DelAttr(obj, name), 0);
  for (int i = 0; i < 2; i++) {
    res = callLoad(cache.get(), obj, name);
    ASSERT_EQ(res.get(), nullptr) << "iteration " << i;
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_AttributeError))
        << "iteration " << i;
    PyErr_Clear();
  }
}

TEST_F(InlineCacheTest, StoreAttrCacheSingleEntryHitsAndMisses) {
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  def __init__(self):
    self.x = 0

class D:
  def __init__(self):
    self.x = 0

c = C()
d = D()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> c = PyDict_GetItemString(locals, "c");
  BorrowedRef<> d = PyDict_GetItemString(locals, "d");
  ASSERT_NE(c.get(), nullptr);
  ASSERT_NE(d.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("x"));
  auto v1 = Ref<>::steal(PyLong_FromLong(41));
  auto v2 = Ref<>::steal(PyLong_FromLong(42));

  CacheStorage<StoreAttrCache> cache;
  for (int i = 0; i < 2; i++) {
    ASSERT_EQ(callStore(cache.get(), c, name, v1), 0) << "iteration " << i;
  }
  auto read = Ref<>::steal(PyObject_GetAttr(c, name));
  ASSERT_NE(read.get(), nullptr);
  EXPECT_EQ(asLong(read), 41);

  // Second type does not fit in a single-entry cache; still must be correct.
  ASSERT_EQ(callStore(cache.get(), d, name, v2), 0);
  read = Ref<>::steal(PyObject_GetAttr(d, name));
  ASSERT_NE(read.get(), nullptr);
  EXPECT_EQ(asLong(read), 42);
}

TEST_F(InlineCacheTest, StoreAttrCacheReportsErrors) {
  getMutableConfig().attr_cache_size = 1;
  auto locals = runToLocals(this, R"(
class C:
  __slots__ = ()

obj = C()
)");
  ASSERT_NE(locals.get(), nullptr);
  BorrowedRef<> obj = PyDict_GetItemString(locals, "obj");
  ASSERT_NE(obj.get(), nullptr);
  auto name = Ref<>::steal(PyUnicode_FromString("nope"));
  auto value = Ref<>::steal(PyLong_FromLong(1));

  CacheStorage<StoreAttrCache> cache;
  EXPECT_LT(callStore(cache.get(), obj, name, value), 0);
  EXPECT_TRUE(PyErr_Occurred());
  PyErr_Clear();
}

} // namespace cinderx
