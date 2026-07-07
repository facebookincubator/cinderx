// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX > 0x030E0000
#include <pycore_unicodeobject.h>
#endif

#include <cstring>

using namespace cinderx::jit;
using namespace cinderx::jit::hir;

class InlineCacheTest : public RuntimeTest {};

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
  // Only add and multiply are currently supported; constructing a cache for any
  // other op kind should throw rather than silently produce a broken cache.
  EXPECT_THROW(BinaryOpCache{BinaryOpKind::kSubtract}, std::runtime_error);
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
