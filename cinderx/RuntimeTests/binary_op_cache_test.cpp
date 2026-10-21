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

TEST_F(BinaryOpCacheCodegenTest, ProfileMergesDuplicateCaches) {
  // Inlining the same callee at two call sites (or recursively into itself)
  // registers two caches under one location. Agreeing observations merge
  // into a single entry; disagreeing ones drop the entry, including any
  // carried payload; a cold duplicate contributes no observation.
  PerCompilationStableStorage storage;
  ProfileCollector collector;

  const ProfileLocation agreeing_location{nullptr, BCOffset{8}};
  BinaryOpCache* first_agreeing =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  BinaryOpCache* second_agreeing =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(
      agreeing_location.code,
      agreeing_location.bytecode_offset,
      first_agreeing);
  collector.add(
      agreeing_location.code,
      agreeing_location.bytecode_offset,
      second_agreeing);
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> first_result =
      Ref<>::steal(BinaryOpCache::add(one, two, first_agreeing));
  ASSERT_NE(first_result, nullptr);
  Ref<> second_result =
      Ref<>::steal(BinaryOpCache::add(one, two, second_agreeing));
  ASSERT_NE(second_result, nullptr);

  const ProfileLocation disagreeing_location{nullptr, BCOffset{16}};
  BinaryOpCache* long_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  BinaryOpCache* float_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(
      disagreeing_location.code,
      disagreeing_location.bytecode_offset,
      long_cache);
  collector.add(
      disagreeing_location.code,
      disagreeing_location.bytecode_offset,
      float_cache);
  collector.add(
      disagreeing_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  Ref<> long_result = Ref<>::steal(BinaryOpCache::add(one, two, long_cache));
  ASSERT_NE(long_result, nullptr);
  auto one_float = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto two_float = Ref<>::steal(PyFloat_FromDouble(2.0));
  Ref<> float_result =
      Ref<>::steal(BinaryOpCache::add(one_float, two_float, float_cache));
  ASSERT_NE(float_result, nullptr);

  const ProfileLocation cold_location{nullptr, BCOffset{24}};
  BinaryOpCache* warm_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  BinaryOpCache* cold_cache = storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(cold_location.code, cold_location.bytecode_offset, warm_cache);
  collector.add(cold_location.code, cold_location.bytecode_offset, cold_cache);
  Ref<> warm_result = Ref<>::steal(BinaryOpCache::add(one, two, warm_cache));
  ASSERT_NE(warm_result, nullptr);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 2);

  const BinaryOpProfile* agreeing =
      profile.find<BinaryOpProfile>(agreeing_location);
  ASSERT_NE(agreeing, nullptr);
  EXPECT_EQ(agreeing->left_type, TLongCompact);
  EXPECT_EQ(agreeing->right_type, TLongCompact);

  EXPECT_EQ(profile.find<BinaryOpProfile>(disagreeing_location), nullptr);

  const BinaryOpProfile* cold = profile.find<BinaryOpProfile>(cold_location);
  ASSERT_NE(cold, nullptr);
  EXPECT_EQ(cold->left_type, TLongCompact);
  EXPECT_EQ(cold->right_type, TLongCompact);
}

TEST_F(BinaryOpCacheCodegenTest, ProfileMergesDuplicateCarriedUses) {
  // The same location reached via multiple inlined ProfileUseTypes records
  // one carried payload per use. Agreeing payloads merge into a single
  // entry; disagreeing ones drop the entry instead of last-write-wins.
  ProfileCollector collector;

  const ProfileLocation agreeing_location{nullptr, BCOffset{8}};
  collector.add(
      agreeing_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  collector.add(
      agreeing_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});

  const ProfileLocation disagreeing_location{nullptr, BCOffset{16}};
  collector.add(
      disagreeing_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  collector.add(
      disagreeing_location,
      ProfileEntry{BinaryOpProfile{TFloatExact, TFloatExact, TFloatExact}});

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 1);

  const BinaryOpProfile* agreeing =
      profile.find<BinaryOpProfile>(agreeing_location);
  ASSERT_NE(agreeing, nullptr);
  EXPECT_EQ(agreeing->left_type, TLong);
  EXPECT_EQ(agreeing->right_type, TLong);

  EXPECT_EQ(profile.find<BinaryOpProfile>(disagreeing_location), nullptr);
}

TEST_F(BinaryOpCacheCodegenTest, ProfileCollectorRemovesEntries) {
  // Removing a location whose profile guard deopted drops both its live
  // caches and its carried uses; other locations are unaffected.
  PerCompilationStableStorage storage;
  ProfileCollector collector;

  const BCOffset stale_offset{8};
  BinaryOpCache* stale_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, stale_offset, stale_cache);
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> stale_result = Ref<>::steal(BinaryOpCache::add(one, two, stale_cache));
  ASSERT_NE(stale_result, nullptr);
  const ProfileLocation stale_location{nullptr, stale_offset};
  collector.add(
      stale_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});

  const BCOffset fresh_offset{16};
  BinaryOpCache* fresh_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, fresh_offset, fresh_cache);
  Ref<> fresh_result = Ref<>::steal(BinaryOpCache::add(one, two, fresh_cache));
  ASSERT_NE(fresh_result, nullptr);

  collector.remove(stale_location);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 1);
  EXPECT_EQ(profile.find<BinaryOpProfile>(stale_location), nullptr);
  EXPECT_NE(profile.find<BinaryOpProfile>(nullptr, fresh_offset), nullptr);

  // Unknown locations are ignored.
  collector.remove(ProfileLocation{nullptr, BCOffset{24}});
}

TEST_F(BinaryOpCacheCodegenTest, ProfileDeoptedLocationsSuppressFindAndMerge) {
  // A removed location stays dead: re-adding it (live caches repopulating,
  // carried uses, or a callee profile merged back in) must not resurrect it
  // via find().
  PerCompilationStableStorage storage;
  ProfileCollector collector;

  const BCOffset stale_offset{8};
  BinaryOpCache* stale_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(nullptr, stale_offset, stale_cache);
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> stale_result = Ref<>::steal(BinaryOpCache::add(one, two, stale_cache));
  ASSERT_NE(stale_result, nullptr);
  const ProfileLocation stale_location{nullptr, stale_offset};
  collector.add(
      stale_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});

  collector.remove(stale_location);

  Profile profile = collector.snapshot();
  EXPECT_TRUE(profile.isDeopted(stale_location));
  EXPECT_EQ(profile.find<BinaryOpProfile>(stale_location), nullptr);

  // Re-adding the same location after the deopt stays suppressed.
  collector.add(
      stale_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  Profile resnapshot = collector.snapshot();
  EXPECT_TRUE(resnapshot.isDeopted(stale_location));
  EXPECT_EQ(resnapshot.find<BinaryOpProfile>(stale_location), nullptr);
  EXPECT_TRUE(
      resnapshot.entries().find(stale_location) == resnapshot.entries().end());

  // Merging a callee profile that still holds the stale entry must not
  // resurrect it either (inlined-guard case).
  ProfileCollector callee;
  callee.add(
      stale_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  Profile callee_profile = callee.snapshot();
  ASSERT_NE(callee_profile.find<BinaryOpProfile>(stale_location), nullptr);
  profile.merge(std::move(callee_profile));
  EXPECT_TRUE(profile.isDeopted(stale_location));
  EXPECT_EQ(profile.find<BinaryOpProfile>(stale_location), nullptr);
  EXPECT_TRUE(
      profile.entries().find(stale_location) == profile.entries().end());
}

TEST_F(BinaryOpCacheCodegenTest, ProfileIndexesComparesByLocation) {
  ProfileCollector collector;
  const BCOffset long_offset{24};
  CompareCache long_cache{CompareOp::kEqual};
  collector.add(nullptr, long_offset, &long_cache);
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> long_result = Ref<>::steal(CompareCache::equal(one, two, &long_cache));
  ASSERT_NE(long_result, nullptr);
  EXPECT_EQ(long_result.get(), Py_False);

  const BCOffset mixed_offset{48};
  CompareCache mixed_cache{CompareOp::kEqual};
  collector.add(nullptr, mixed_offset, &mixed_cache);
  auto mixed_str = Ref<>::steal(PyUnicode_FromString("x"));
  Ref<> mixed_result =
      Ref<>::steal(CompareCache::equal(one, mixed_str, &mixed_cache));
  ASSERT_NE(mixed_result, nullptr);
  EXPECT_EQ(mixed_result.get(), Py_False);

  const BCOffset float_offset{12};
  CompareCache float_cache{CompareOp::kEqual};
  collector.add(nullptr, float_offset, &float_cache);
  auto one_float = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto two_float = Ref<>::steal(PyFloat_FromDouble(2.0));
  Ref<> float_result =
      Ref<>::steal(CompareCache::equal(one_float, two_float, &float_cache));
  ASSERT_NE(float_result, nullptr);
  EXPECT_EQ(float_result.get(), Py_False);

  const BCOffset unicode_offset{6};
  CompareCache unicode_cache{CompareOp::kEqual};
  collector.add(nullptr, unicode_offset, &unicode_cache);
  auto left_unicode = Ref<>::steal(PyUnicode_FromString("left"));
  auto right_unicode = Ref<>::steal(PyUnicode_FromString("right"));
  Ref<> unicode_result = Ref<>::steal(
      CompareCache::equal(left_unicode, right_unicode, &unicode_cache));
  ASSERT_NE(unicode_result, nullptr);
  EXPECT_EQ(unicode_result.get(), Py_False);

  const BCOffset cold_offset{60};
  CompareCache cold_cache{CompareOp::kEqual};
  collector.add(nullptr, cold_offset, &cold_cache);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 3);

  const CompareProfile* long_profile =
      profile.find<CompareProfile>(nullptr, long_offset);
  ASSERT_NE(long_profile, nullptr);
  EXPECT_EQ(long_profile->left_type, TLongCompact);
  EXPECT_EQ(long_profile->right_type, TLongCompact);
  EXPECT_EQ(long_profile->res_type, TBool);

  const CompareProfile* float_profile =
      profile.find<CompareProfile>(nullptr, float_offset);
  ASSERT_NE(float_profile, nullptr);
  EXPECT_EQ(float_profile->left_type, TFloatExact);
  EXPECT_EQ(float_profile->right_type, TFloatExact);
  EXPECT_EQ(float_profile->res_type, TBool);

  const CompareProfile* unicode_profile =
      profile.find<CompareProfile>(nullptr, unicode_offset);
  ASSERT_NE(unicode_profile, nullptr);
  EXPECT_EQ(unicode_profile->left_type, TUnicodeExact);
  EXPECT_EQ(unicode_profile->right_type, TUnicodeExact);
  EXPECT_EQ(unicode_profile->res_type, TBool);

  // Mixed operand types go generic, so the site produces no profile entry.
  EXPECT_EQ(profile.find<CompareProfile>(nullptr, mixed_offset), nullptr);
  // A cache that was never called produces no profile entry either.
  EXPECT_EQ(profile.find<CompareProfile>(nullptr, cold_offset), nullptr);
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

TEST_F(BinaryOpCacheCodegenTest, RecompileSpecializesFromCompareProfile) {
  const char* src = R"(
def test(a, b):
  return a == b
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
  EXPECT_EQ(result.get(), Py_False);

  Context* ctx = getContext();
  ASSERT_NE(ctx, nullptr);
  BorrowedRef<CompiledFunction> first = ctx->lookupFunc(func);
  ASSERT_NE(first, nullptr);
  Profile profile = first->snapshotProfile();
  ASSERT_EQ(profile.entries().size(), 1);
  const CompareProfile* compare_profile =
      profile.find<CompareProfile>(profile.entries().begin()->first);
  ASSERT_NE(compare_profile, nullptr);
  EXPECT_EQ(compare_profile->left_type, TLongCompact);
  EXPECT_EQ(compare_profile->right_type, TLongCompact);
  EXPECT_EQ(compare_profile->res_type, TBool);

  CompiledFunctionData recompiled = recompileWithProfile(func, profile);
  const hir::OpcodeCounts& counts = recompiled.hir_opcode_counts;
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kCompare)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongCompare)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kPrimitiveCompare)], 1);

  auto recompiled_result = Ref<>::steal(recompiled.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(recompiled_result, nullptr);
  EXPECT_EQ(recompiled_result.get(), Py_False);
}

TEST_F(BinaryOpCacheCodegenTest, ProfileMergesCarriedPayloads) {
  PerCompilationStableStorage storage;
  ProfileCollector collector;

  // A carried-only location is emitted as-is.
  const ProfileLocation carried_location{nullptr, BCOffset{8}};
  collector.add(
      carried_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});

  // A carried payload agreeing with a live cache collapses to one entry.
  const ProfileLocation agreeing_location{nullptr, BCOffset{16}};
  BinaryOpCache* agreeing_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(
      agreeing_location.code,
      agreeing_location.bytecode_offset,
      agreeing_cache);
  collector.add(
      agreeing_location,
      ProfileEntry{BinaryOpProfile{TLongCompact, TLongCompact, TLongCompact}});
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> agreeing_result =
      Ref<>::steal(BinaryOpCache::add(one, two, agreeing_cache));
  ASSERT_NE(agreeing_result, nullptr);

  // A live cache overwrites a conflicting carried payload.
  const ProfileLocation conflicting_location{nullptr, BCOffset{24}};
  BinaryOpCache* conflicting_cache =
      storage.allocateBinaryOpCache(BinaryOpKind::kAdd);
  collector.add(
      conflicting_location.code,
      conflicting_location.bytecode_offset,
      conflicting_cache);
  collector.add(
      conflicting_location, ProfileEntry{BinaryOpProfile{TLong, TLong, TLong}});
  auto one_float = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto two_float = Ref<>::steal(PyFloat_FromDouble(2.0));
  Ref<> conflicting_result =
      Ref<>::steal(BinaryOpCache::add(one_float, two_float, conflicting_cache));
  ASSERT_NE(conflicting_result, nullptr);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 3);

  const BinaryOpProfile* carried =
      profile.find<BinaryOpProfile>(carried_location);
  ASSERT_NE(carried, nullptr);
  EXPECT_EQ(carried->left_type, TLong);
  EXPECT_EQ(carried->right_type, TLong);

  const BinaryOpProfile* agreeing =
      profile.find<BinaryOpProfile>(agreeing_location);
  ASSERT_NE(agreeing, nullptr);
  EXPECT_EQ(agreeing->left_type, TLongCompact);
  EXPECT_EQ(agreeing->right_type, TLongCompact);

  const BinaryOpProfile* conflicting =
      profile.find<BinaryOpProfile>(conflicting_location);
  ASSERT_NE(conflicting, nullptr);
  EXPECT_EQ(conflicting->left_type, TFloatExact);
  EXPECT_EQ(conflicting->right_type, TFloatExact);
}

TEST_F(BinaryOpCacheCodegenTest, ProfileMergesCarriedComparePayloads) {
  ProfileCollector collector;

  // A carried-only location is emitted as-is.
  const ProfileLocation carried_location{nullptr, BCOffset{8}};
  collector.add(
      carried_location, ProfileEntry{CompareProfile{TLong, TLong, TBool}});

  // A carried payload agreeing with a live cache collapses to one entry.
  const ProfileLocation agreeing_location{nullptr, BCOffset{16}};
  CompareCache agreeing_cache{CompareOp::kEqual};
  collector.add(
      agreeing_location.code,
      agreeing_location.bytecode_offset,
      &agreeing_cache);
  collector.add(
      agreeing_location,
      ProfileEntry{CompareProfile{TLongCompact, TLongCompact, TBool}});
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  Ref<> agreeing_result =
      Ref<>::steal(CompareCache::equal(one, two, &agreeing_cache));
  ASSERT_NE(agreeing_result, nullptr);

  // A live cache overwrites a conflicting carried payload.
  const ProfileLocation conflicting_location{nullptr, BCOffset{24}};
  CompareCache conflicting_cache{CompareOp::kEqual};
  collector.add(
      conflicting_location.code,
      conflicting_location.bytecode_offset,
      &conflicting_cache);
  collector.add(
      conflicting_location, ProfileEntry{CompareProfile{TLong, TLong, TBool}});
  auto one_float = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto two_float = Ref<>::steal(PyFloat_FromDouble(2.0));
  Ref<> conflicting_result = Ref<>::steal(
      CompareCache::equal(one_float, two_float, &conflicting_cache));
  ASSERT_NE(conflicting_result, nullptr);

  Profile profile = collector.snapshot();
  ASSERT_EQ(profile.entries().size(), 3);

  const CompareProfile* carried =
      profile.find<CompareProfile>(carried_location);
  ASSERT_NE(carried, nullptr);
  EXPECT_EQ(carried->left_type, TLong);
  EXPECT_EQ(carried->right_type, TLong);
  EXPECT_EQ(carried->res_type, TBool);

  const CompareProfile* agreeing =
      profile.find<CompareProfile>(agreeing_location);
  ASSERT_NE(agreeing, nullptr);
  EXPECT_EQ(agreeing->left_type, TLongCompact);
  EXPECT_EQ(agreeing->right_type, TLongCompact);
  EXPECT_EQ(agreeing->res_type, TBool);

  const CompareProfile* conflicting =
      profile.find<CompareProfile>(conflicting_location);
  ASSERT_NE(conflicting, nullptr);
  EXPECT_EQ(conflicting->left_type, TFloatExact);
  EXPECT_EQ(conflicting->right_type, TFloatExact);
  EXPECT_EQ(conflicting->res_type, TBool);
}

TEST_F(BinaryOpCacheCodegenTest, RecompilePersistsProfileAcrossRecompiles) {
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
  const hir::OpcodeCounts& first_counts = first->hirOpcodeCounts();
  EXPECT_EQ(first_counts[static_cast<size_t>(Opcode::kBinaryOp)], 1);
  EXPECT_EQ(first_counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 0);

  Profile profile = first->snapshotProfile();
  ASSERT_EQ(profile.entries().size(), 1);

  CompiledFunctionData second = recompileWithProfile(func, profile);
  const hir::OpcodeCounts& counts = second.hir_opcode_counts;
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kBinaryOp)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongBinaryOp)], 1);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 1);

  auto recompiled_result = Ref<>::steal(second.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(recompiled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(recompiled_result), 3);

  // The recompiled code has no BinaryOp cache left, so this entry can only
  // come from the carried payload.
  ASSERT_NE(second.profile_collector, nullptr);
  Profile carried_profile = second.profile_collector->snapshot();
  ASSERT_EQ(carried_profile.entries().size(), 1);
  const BinaryOpProfile* carried = carried_profile.find<BinaryOpProfile>(
      carried_profile.entries().begin()->first);
  ASSERT_NE(carried, nullptr);
  EXPECT_EQ(carried->left_type, TLongCompact);
  EXPECT_EQ(carried->right_type, TLongCompact);

  CompiledFunctionData third = recompileWithProfile(func, carried_profile);
  const hir::OpcodeCounts& third_counts = third.hir_opcode_counts;
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kBinaryOp)], 0);
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kLongBinaryOp)], 1);
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 1);

  auto rerecompiled_result = Ref<>::steal(third.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(rerecompiled_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(rerecompiled_result), 3);
}

TEST_F(
    BinaryOpCacheCodegenTest,
    RecompilePersistsCompareProfileAcrossRecompiles) {
  const char* src = R"(
def test(a, b):
  return a == b
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
  EXPECT_EQ(result.get(), Py_False);

  Context* ctx = getContext();
  ASSERT_NE(ctx, nullptr);
  BorrowedRef<CompiledFunction> first = ctx->lookupFunc(func);
  ASSERT_NE(first, nullptr);
  const hir::OpcodeCounts& first_counts = first->hirOpcodeCounts();
  EXPECT_EQ(first_counts[static_cast<size_t>(Opcode::kCompare)], 1);
  EXPECT_EQ(first_counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 0);

  Profile profile = first->snapshotProfile();
  ASSERT_EQ(profile.entries().size(), 1);

  CompiledFunctionData second = recompileWithProfile(func, profile);
  const hir::OpcodeCounts& counts = second.hir_opcode_counts;
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kCompare)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongCompare)], 0);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kPrimitiveCompare)], 1);
  EXPECT_EQ(counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 1);

  auto recompiled_result = Ref<>::steal(second.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(recompiled_result, nullptr);
  EXPECT_EQ(recompiled_result.get(), Py_False);

  // The recompiled code has no Compare cache left, so this entry can only
  // come from the carried payload.
  ASSERT_NE(second.profile_collector, nullptr);
  Profile carried_profile = second.profile_collector->snapshot();
  ASSERT_EQ(carried_profile.entries().size(), 1);
  const CompareProfile* carried = carried_profile.find<CompareProfile>(
      carried_profile.entries().begin()->first);
  ASSERT_NE(carried, nullptr);
  EXPECT_EQ(carried->left_type, TLongCompact);
  EXPECT_EQ(carried->right_type, TLongCompact);
  EXPECT_EQ(carried->res_type, TBool);

  CompiledFunctionData third = recompileWithProfile(func, carried_profile);
  const hir::OpcodeCounts& third_counts = third.hir_opcode_counts;
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kCompare)], 0);
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kLongCompare)], 0);
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kPrimitiveCompare)], 1);
  EXPECT_EQ(third_counts[static_cast<size_t>(Opcode::kProfileUseTypes)], 1);

  auto rerecompiled_result = Ref<>::steal(third.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), args, 2, nullptr));
  ASSERT_NE(rerecompiled_result, nullptr);
  EXPECT_EQ(rerecompiled_result.get(), Py_False);
}

TEST_F(BinaryOpCacheCodegenTest, ProfileDropsEntryOnGuardFailure) {
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

  CompiledFunctionData recompiled = recompileWithProfile(func, profile);

  // Contradicting types fail the profile guards and deopt back to the
  // interpreter. The recompiled code is never installed, so the deopt
  // attributes the stale entry to the installed compilation and drops it
  // from that collector.
  auto left_float = Ref<>::steal(PyFloat_FromDouble(1.5));
  auto right_float = Ref<>::steal(PyFloat_FromDouble(2.5));
  PyObject* float_args[] = {left_float.get(), right_float.get()};
  auto float_result = Ref<>::steal(recompiled.vectorcall_entry(
      reinterpret_cast<PyObject*>(func.get()), float_args, 2, nullptr));
  ASSERT_NE(float_result, nullptr);
  EXPECT_EQ(PyFloat_AsDouble(float_result), 4.0);

  Profile after = first->snapshotProfile();
  EXPECT_TRUE(after.entries().empty());
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
