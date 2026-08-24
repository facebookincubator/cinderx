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

#include <cstring>

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
