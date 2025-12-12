// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/genobject_jit.h"
#endif

#include "cinderx/Common/slab_arena.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/fixed_type_profiler.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/runtime_iface.h"
#include "cinderx/Jit/type_deopt_patchers.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {

#if PY_VERSION_HEX < 0x030C0000
// Memory management functions for JIT generator data.
// In 3.12+ there is no gen->gi_jit_data and this functionality is part of
// JitGenObject.

jit::GenDataFooter* jitgen_data_allocate(size_t spill_words);
void jitgen_data_free(PyGenObject* gen);

inline GenDataFooter* genDataFooter(PyGenObject* gen) {
  return reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
}

// The number of words for pre-allocated blocks in the generator suspend data
// free-list. I chose this based on it covering 99% of the JIT generator
// spill-sizes needed when running 'make testcinder_jit' at the time I collected
// this data. For reference:
//   99.9% coverage came at 256 spill size
//   99.99% was at 1552
//   max was 4999
// There were about ~15k JIT generators in total during the run.
constexpr size_t kMinGenSpillWords = 89;

// Pre 3.12 these fields needed to be at a fixed offset so they can be quickly
// accessed from C code in genobject.c.
static_assert(
    offsetof(GenDataFooter, state) == Ci_GEN_JIT_DATA_OFFSET_STATE,
    "Byte offset for state shifted");
static_assert(
    offsetof(GenDataFooter, yieldPoint) == Ci_GEN_JIT_DATA_OFFSET_YIELD_POINT,
    "Byte offset for yieldPoint shifted");

#endif

PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point);

// Information about the runtime behavior of a single deopt point: how often
// it's been hit, and the frequency of guilty types, if applicable.
struct DeoptStat {
  std::size_t count;
  FixedTypeProfiler<4> types;
};

// Map from CodeRuntime to stats about each deopt point.
//
// Uses an unordered map to store the deopt stats for each code object as it's
// meant to be sparse.  We expect most deopt points to be unused.
using DeoptStats = jit::
    UnorderedMap<const CodeRuntime*, jit::UnorderedMap<std::size_t, DeoptStat>>;

using InlineCacheStats = std::vector<CacheStats>;

class Builtins {
 public:
  void init();
  bool isInitialized() const;
  std::optional<std::string> find(PyMethodDef* meth) const;
  std::optional<PyMethodDef*> find(const std::string& name) const;

 private:
  std::atomic<bool> is_initialized_{false};
  UnorderedMap<PyMethodDef*, std::string> cfunc_to_name_;
  UnorderedMap<std::string, PyMethodDef*> name_to_cfunc_;
};

// Runtime owns all metadata created by the JIT.
class Runtime : public IRuntime {
 public:
  // Return the singleton Runtime, creating it first if necessary.
  static Runtime* get();

  // Return the singleton Runtime, if it exists.
  static Runtime* getUnchecked();

  Runtime();

  template <typename... Args>
  CodeRuntime* allocateCodeRuntime(Args&&... args) {
    return code_runtimes_.allocate(std::forward<Args>(args)...);
  }

  void mlockProfilerDependencies();

  // Find a cache for the indirect static entry point for a function.
  void** findFunctionEntryCache(PyFunctionObject* function);

  // Checks to see if we already have an entry for indirect static entry point
  bool hasFunctionEntryCache(PyFunctionObject* function) const;

  // Gets information about the primitive arguments that a function
  // is typed to.  Typed object references are explicitly excluded.
  _PyTypedArgsInfo* findFunctionPrimitiveArgInfo(PyFunctionObject* function);

  // Record that a deopt of the given index happened at runtime, with an
  // optional guilty value.
  void recordDeopt(
      CodeRuntime* code_runtime,
      std::size_t idx,
      BorrowedRef<> guilty_value);

  // Get the stat object for a given deopt.  It will not exist if the deopt has
  // never been hit.
  const DeoptStat* deoptStat(
      const CodeRuntime* code_runtime,
      std::size_t deopt_idx) const;

  // Clear all deopt stats.
  void clearDeoptStats();

  // Get and clear inline cache stats.
  InlineCacheStats getAndClearLoadMethodCacheStats();
  InlineCacheStats getAndClearLoadTypeMethodCacheStats();

  using GuardFailureCallback = std::function<void(const DeoptMetadata&)>;

  // Add a function to be called when deoptimization occurs due to guard
  // failure. Intended to be used for testing/debugging only.
  void setGuardFailureCallback(GuardFailureCallback cb);
  void guardFailed(const DeoptMetadata& deopt_meta);
  void clearGuardFailureCallback();

  // Ensure that this Runtime owns a reference to the given borrowed object,
  // keeping it alive for use by the compiled code. Make CodeRuntime a new
  // owner of the object.
  void addReference(BorrowedRef<> obj);

  // Release any references this Runtime holds to Python objects.
  void releaseReferences();

  // Allocate a new attribute cache.
  LoadAttrCache* allocateLoadAttrCache();
  LoadTypeAttrCache* allocateLoadTypeAttrCache();
  LoadMethodCache* allocateLoadMethodCache();
  LoadModuleAttrCache* allocateLoadModuleAttrCache();
  LoadModuleMethodCache* allocateLoadModuleMethodCache();
  LoadTypeMethodCache* allocateLoadTypeMethodCache();
  StoreAttrCache* allocateStoreAttrCache();

  const Builtins& builtins();

  // Some profilers need to walk the code_rt->code->qualname chain for jitted
  // functions on the call stack. The JIT rarely touches this memory and, as a
  // result, the OS may page it out. Out of process profilers (i.e. those that
  // use eBPF) that attempt to read the memory after it has been paged out will
  // fail; the read would cause a page fault which is currently unsupported
  // inside of an eBPF probe. Periodically calling this function will ensure
  // that the OS doesn't page out the memory too aggressively.
  //
  // Returns a PyListObject containing the qualnames of the units for which
  // memory was paged in.
  Ref<> pageInProfilerDependencies();

  // When type is modified or an instance of type has __class__ assigned to,
  // call patcher->maybePatch(new_ty).
  void watchType(BorrowedRef<PyTypeObject> type, TypeDeoptPatcher* patcher);

  // Callback for when a type is modified or destroyed. lookup_type should be
  // the type that triggered the call (the type that's being
  // modified/deleted/otherwise messed with), and new_type should be the "new"
  // type that is taking its place.
  //
  // In the case of a modification, this new type will be the same as
  // lookup_type, and for type destruction it will be nullptr. For __class__
  // assignment, it will be the new type assigned to the object, in case the
  // deopt patcher determines that the new type is still suitable for the
  // specialized code.
  void notifyTypeModified(
      BorrowedRef<PyTypeObject> lookup_type,
      BorrowedRef<PyTypeObject> new_type);

#if PY_VERSION_HEX < 0x030C0000
  // In 3.12+ the equivalent of this is in generators_rt.cpp.
  template <typename F>
    requires std::is_invocable_r_v<int, F, PyObject*>
  int forEachOwnedRef(PyGenObject* gen, const DeoptMetadata& meta, F func) {
    auto base = reinterpret_cast<char*>(genDataFooter(gen));
    for (const LiveValue& value : meta.live_values) {
      if (value.ref_kind != hir::RefKind::kOwned) {
        continue;
      }
      codegen::PhyLocation loc = value.location;
      JIT_CHECK(
          !loc.is_register(),
          "DeoptMetadata for Yields should not reference registers");
      int ret = func(*reinterpret_cast<PyObject**>(base + loc.loc));
      if (ret != 0) {
        return ret;
      }
    }
    return 0;
  }
#endif

  BorrowedRef<> zero() override;
  BorrowedRef<> strBuildClass();

  void watchPendingTypes();
  void fixupFunctionEntryCachePostMultiThreadedCompile();

  const hir::Type& typeForCommonConstant(int i) const;

  // Allocate all CodeRuntimes together so they can be mlocked() without
  // including any other data that happened to be on the same page.
  SlabArena<CodeRuntime> code_runtimes_;

  // These SlabAreas hold data that is allocated at compile-time and likely to
  // change at runtime, and should be isolated from other data to avoid COW
  // casualties.
  SlabArena<LoadAttrCache, AttributeCacheSizeTrait> load_attr_caches_;
  SlabArena<LoadTypeAttrCache> load_type_attr_caches_;
  SlabArena<LoadMethodCache> load_method_caches_;
  SlabArena<LoadModuleAttrCache> load_module_attr_caches_;
  SlabArena<LoadModuleMethodCache> load_module_method_caches_;
  SlabArena<LoadTypeMethodCache> load_type_method_caches_;
  SlabArena<StoreAttrCache, AttributeCacheSizeTrait> store_attr_caches_;
  SlabArena<void*> pointer_caches_;

  FunctionEntryCacheMap function_entry_caches_;

  std::vector<DeoptMetadata> deopt_metadata_;
  DeoptStats deopt_stats_;
  GuardFailureCallback guard_failure_callback_;

  // References to Python objects held by this Runtime
  std::unordered_set<ThreadedRef<PyObject>> references_;
  Builtins builtins_;

  std::unordered_map<BorrowedRef<PyTypeObject>, std::vector<TypeDeoptPatcher*>>
      type_deopt_patchers_;

  Ref<> zero_;
  Ref<> str_build_class_;
  std::unordered_set<BorrowedRef<PyTypeObject>> pending_watches_;

  std::vector<hir::Type> common_constant_types_;
};

} // namespace jit
