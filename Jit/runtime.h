// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>
#if PY_VERSION_HEX < 0x030C0000
#include "cinder/genobject_jit.h"
#endif

#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/fixed_type_profiler.h"
#include "cinderx/Jit/global_cache.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/slab_arena.h"
#include "cinderx/Jit/symbolizer.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/Jit/type_deopt_patchers.h"
#include "cinderx/Upgrade/upgrade_assert.h" // @donotremove
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {

class GenYieldPoint {
 public:
  explicit GenYieldPoint(
      std::size_t deopt_idx,
      bool is_yield_from,
      ptrdiff_t yield_from_offs)
      : deopt_idx_(deopt_idx),
        isYieldFrom_(is_yield_from),
        yieldFromOffs_(yield_from_offs) {}

  void setResumeTarget(uint64_t resume_target) {
    resume_target_ = resume_target;
  }

  uint64_t resumeTarget() const {
    return resume_target_;
  }

  std::size_t deoptIdx() const {
    return deopt_idx_;
  }

  bool isYieldFrom() const {
    return isYieldFrom_;
  }

  ptrdiff_t yieldFromOffset() const {
    return yieldFromOffs_;
  }

  static constexpr int resumeTargetOffset() {
    return offsetof(GenYieldPoint, resume_target_);
  }

 private:
  uint64_t resume_target_{0};
  const std::size_t deopt_idx_;
  const bool isYieldFrom_;
  const ptrdiff_t yieldFromOffs_;
};

class alignas(16) RuntimeFrameState {
 public:
  RuntimeFrameState(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<> builtins,
      BorrowedRef<> globals)
      : code_(code), builtins_(builtins), globals_(globals) {}

  bool isGen() const {
    return code()->co_flags & kCoFlagsAnyGenerator;
  }

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  BorrowedRef<> builtins() const {
    return builtins_;
  }

  BorrowedRef<> globals() const {
    return globals_;
  }

  static constexpr int64_t codeOffset() {
    return offsetof(RuntimeFrameState, code_);
  }

 private:
  // These are owned by the CodeRuntime that owns this RuntimeFrameState.
  BorrowedRef<PyCodeObject> code_;
  BorrowedRef<> builtins_;
  BorrowedRef<> globals_;
};

// Runtime data for a PyCodeObject object, containing caches and any other data
// associated with a JIT-compiled function.
class alignas(16) CodeRuntime {
 public:
  explicit CodeRuntime(
      PyCodeObject* code,
      PyObject* builtins,
      PyObject* globals)
      : frame_state_(code, builtins, globals) {
    // TODO(T88040922): Until we work out something smarter, force code,
    // globals, and builtins objects for compiled functions to live as long as
    // the JIT is initialized.
    addReference(BorrowedRef(code));
    addReference(builtins);
    addReference(globals);
  }

  CodeRuntime(PyFunctionObject* func)
      : CodeRuntime(
            reinterpret_cast<PyCodeObject*>(func->func_code),
            func->func_builtins,
            func->func_globals) {}

  template <typename... Args>
  RuntimeFrameState* allocateRuntimeFrameState(Args&&... args) {
    // Serialize as we modify the globally shared runtimes data.
    ThreadedCompileSerialize guard;
    inlined_frame_states_.emplace_back(
        std::make_unique<RuntimeFrameState>(std::forward<Args>(args)...));
    return inlined_frame_states_.back().get();
  }

  const RuntimeFrameState* frameState() const {
    return &frame_state_;
  }

  // Release any references this CodeRuntime holds to Python objects.
  void releaseReferences();

  // Ensure that this CodeRuntime owns a reference to the given owned object,
  // keeping it alive for use by the compiled code. Transfer ownership of the
  // object to the CodeRuntime.
  void addReference(Ref<>&& obj);

  // Ensure that this CodeRuntime owns a reference to the given borrowed
  // object, keeping it alive for use by the compiled code. Make CodeRuntime a
  // new owner of the object.
  void addReference(BorrowedRef<> obj);

  // Store meta-data about generator yield point.
  GenYieldPoint* addGenYieldPoint(GenYieldPoint&& gen_yield_point) {
    gen_yield_points_.emplace_back(std::move(gen_yield_point));
    return &gen_yield_points_.back();
  }

  void set_frame_size(int size) {
    frame_size_ = size;
  }
  int frame_size() const {
    return frame_size_;
  }

  DebugInfo* debug_info() {
    return &debug_info_;
  }

  static constexpr int64_t frameStateOffset() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    return offsetof(CodeRuntime, frame_state_);
#pragma GCC diagnostic pop
  }

  static const int64_t kPyCodeOffset;

 private:
  RuntimeFrameState frame_state_;
  std::vector<std::unique_ptr<RuntimeFrameState>> inlined_frame_states_;

  std::unordered_set<Ref<PyObject>> references_;

  // Metadata about yield points. Deque so we can have raw pointers to content.
  std::deque<GenYieldPoint> gen_yield_points_;

  int frame_size_{-1};

  DebugInfo debug_info_;
};

// In a regular JIT function spill-data is stored at negative offsets from RBP
// and RBP points into the system stack. In JIT generators spilled data is still
// stored backwards from RBP, but RBP points to a heap allocated block and this
// persists when the generator is suspended.
//
// While the content of spill data is arbitrary depending on the function, we
// also have a few items of data about the current generator we want to access
// quickly. We can do this via positive offsets from RBP into the
// GenDataFooter struct defined below.
//
// Together the spill data and GenDataFooter make up the complete JIT-specific
// data needed for a generator. PyGenObject::gi_jit_data points above the _top_
// of the spill data (i.e. at the start of the footer). This allows us to
// easily set RBP to the pointer value on generator resume.
//
// The base address of the complete heap allocated suspend data is:
//   PyGenObject::gi_jit_data - GenDataFooter::spillWords
//
// TODO(T209500214): In 3.12 we should roll this data directly into memory
// allocated for a generator rather than having it in a separate heap object.
struct GenDataFooter {
  // Tools which examine/walk the stack expect the following two values to be
  // ahead of RBP.
  uint64_t linkAddress;
  uint64_t returnAddress;

  // RBP that was swapped out to point to this spill-data.
  uint64_t originalRbp;

  // Static data specific to the current yield point. Only non-null when we are
  // suspended.
  GenYieldPoint* yieldPoint;

#if PY_VERSION_HEX < 0x030C0000
  // Current overall state of the JIT.
  // In 3.12+ we use the new PyGenObject::gi_frame_state field instead.
  CiJITGenState state;
#endif

  // Allocated space before this struct in 64-bit words.
  size_t spillWords;

  // Entry-point to resume a JIT generator.
  GenResumeFunc resumeEntry;

  // Associated generator object
  PyGenObject* gen;

  // JIT metadata for associated code object
  CodeRuntime* code_rt{nullptr};
};

// Memory management functions for JIT generator data.
// TODO(T209500214): Eliminate the need for these functions in 3.12+.
jit::GenDataFooter* jitgen_data_allocate(size_t spill_words);
#if PY_VERSION_HEX < 0x030C0000
void jitgen_data_free(PyGenObject* gen);
#else
// Using GenDataFooter directly in 3.12+ avoids a cyclic dependency on the
// generators-rt library.
void jitgen_data_free(GenDataFooter* gen_data_footer);
#endif

#if PY_VERSION_HEX < 0x030C0000
// In 3.12+ there is no gen->gi_jit_data and this functionality is part of
// JitGenObject.
inline GenDataFooter* genDataFooter(PyGenObject* gen) {
  return reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
}

// Pre 3.12 these fields needed to be at a fixed offset so they can be quickly
// accessed from C code in genobject.c.
static_assert(
    offsetof(GenDataFooter, state) == Ci_GEN_JIT_DATA_OFFSET_STATE,
    "Byte offset for state shifted");
static_assert(
    offsetof(GenDataFooter, yieldPoint) == Ci_GEN_JIT_DATA_OFFSET_YIELD_POINT,
    "Byte offset for yieldPoint shifted");
#endif

inline PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point) {
  return yield_point->isYieldFrom()
      ? reinterpret_cast<PyObject*>(
            *(reinterpret_cast<uint64_t*>(gen_footer) +
              yield_point->yieldFromOffset()))
      : nullptr;
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

// Information about the runtime behavior of a single deopt point: how often
// it's been hit, and the frequency of guilty types, if applicable.
struct DeoptStat {
  std::size_t count;
  FixedTypeProfiler<4> types;
};

// Map from DeoptMetadata index to stats about that deopt point.
using DeoptStats = jit::UnorderedMap<std::size_t, DeoptStat>;

using InlineCacheStats = std::vector<CacheStats>;

class Builtins {
 public:
  void init();
  bool isInitialized() const {
    return is_initialized_;
  }
  std::optional<std::string> find(PyMethodDef* meth) const;
  std::optional<PyMethodDef*> find(const std::string& name) const;

 private:
  std::atomic<bool> is_initialized_{false};
  UnorderedMap<PyMethodDef*, std::string> cfunc_to_name_;
  UnorderedMap<std::string, PyMethodDef*> name_to_cfunc_;
};

// Runtime owns all metadata created by the JIT.
class Runtime {
 public:
  // Return the singleton Runtime, creating it first if necessary.
  static Runtime* get() {
    if (s_runtime_ == nullptr) {
      s_runtime_ = new Runtime();
    }
    return s_runtime_;
  }

  // Return the singleton Runtime, if it exists.
  static Runtime* getUnchecked() {
    return s_runtime_;
  }

  // Destroy the singleton Runtime, performing any related cleanup as needed.
  static void shutdown();

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

  // Add metadata used during deopt. Returns a handle that can be used to
  // fetch the metadata from generated code.
  std::size_t addDeoptMetadata(DeoptMetadata&& deopt_meta);

  // Get a reference to the DeoptMetadata with the given id. If this function is
  // called from a context where a threaded compile may be active, the caller is
  // responsible for holding the threaded compile lock for the lifetime of the
  // returned reference.
  DeoptMetadata& getDeoptMetadata(std::size_t id);

  // Record that a deopt of the given index happened at runtime, with an
  // optional guilty value.
  void recordDeopt(std::size_t idx, BorrowedRef<> guilty_value);

  // Get and/or clear runtime deopt stats.
  const DeoptStats& deoptStats() const;
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

  // Ensure that this Runtime owns a reference to the given owned object,
  // keeping it alive for use by the compiled code. Transfer ownership of the
  // object to the CodeRuntime.
  void addReference(Ref<>&& obj);

  // Ensure that this Runtime owns a reference to the given borrowed object,
  // keeping it alive for use by the compiled code. Make CodeRuntime a new
  // owner of the object.
  void addReference(BorrowedRef<> obj);

  // Release any references this Runtime holds to Python objects.
  void releaseReferences();

  template <typename T, typename... Args>
  T* allocateDeoptPatcher(Args&&... args) {
    deopt_patchers_.emplace_back(
        std::make_unique<T>(std::forward<Args>(args)...));
    return static_cast<T*>(deopt_patchers_.back().get());
  }

  LoadAttrCache* allocateLoadAttrCache() {
    return load_attr_caches_.allocate();
  }

  LoadTypeAttrCache* allocateLoadTypeAttrCache() {
    return load_type_attr_caches_.allocate();
  }

  LoadMethodCache* allocateLoadMethodCache() {
    return load_method_caches_.allocate();
  }

  LoadModuleMethodCache* allocateLoadModuleMethodCache() {
    return load_module_method_caches_.allocate();
  }

  LoadTypeMethodCache* allocateLoadTypeMethodCache() {
    return load_type_method_caches_.allocate();
  }

  StoreAttrCache* allocateStoreAttrCache() {
    return store_attr_caches_.allocate();
  }

  const Builtins& builtins() {
    // Lock-free fast path followed by single-lock slow path during
    // initialization.
    if (!builtins_.isInitialized()) {
      builtins_.init();
    }
    return builtins_;
  }

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
  REQUIRES_CALLABLE(F, int, PyObject*)
  int forEachOwnedRef(PyGenObject* gen, std::size_t deopt_idx, F func) {
    const DeoptMetadata& meta = getDeoptMetadata(deopt_idx);
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

 private:
  static Runtime* s_runtime_;

  // Allocate all CodeRuntimes together so they can be mlocked() without
  // including any other data that happened to be on the same page.
  SlabArena<CodeRuntime> code_runtimes_;

  // These SlabAreas hold data that is allocated at compile-time and likely to
  // change at runtime, and should be isolated from other data to avoid COW
  // casualties.
  SlabArena<LoadAttrCache, AttributeCacheSizeTrait> load_attr_caches_;
  SlabArena<LoadTypeAttrCache> load_type_attr_caches_;
  SlabArena<LoadMethodCache> load_method_caches_;
  SlabArena<LoadModuleMethodCache> load_module_method_caches_;
  SlabArena<LoadTypeMethodCache> load_type_method_caches_;
  SlabArena<StoreAttrCache, AttributeCacheSizeTrait> store_attr_caches_;
  SlabArena<void*> pointer_caches_;

  FunctionEntryCacheMap function_entry_caches_;

  std::vector<DeoptMetadata> deopt_metadata_;
  DeoptStats deopt_stats_;
  GuardFailureCallback guard_failure_callback_;

  // References to Python objects held by this Runtime
  std::unordered_set<Ref<PyObject>> references_;
  std::vector<std::unique_ptr<DeoptPatcher>> deopt_patchers_;
  Builtins builtins_;

  std::unordered_map<BorrowedRef<PyTypeObject>, std::vector<TypeDeoptPatcher*>>
      type_deopt_patchers_;
};

} // namespace jit
