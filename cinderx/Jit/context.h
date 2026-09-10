// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/containers.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/compilation_lock.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/context_iface.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/elf/note.h"
#include "cinderx/Jit/fixed_type_profiler.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/inline_cache_storage.h"
#include "cinderx/Jit/nested_compile.h"
#include "cinderx/Jit/pyjit_result.h"
#include "cinderx/Jit/type_deopt_patchers.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cinderx::jit {

// Only used to serialize FT-only entrypoints, but declared unconditionally so
// callers can branch on kFreeThreadedBuild instead of the preprocessor.
std::recursive_mutex& freeThreadedJITEntrypointMutex();

// Free-threaded builds can enter top-level JIT operations concurrently:
// function/code registration, compilation, and destruction hooks.
// Use a dedicated lock instead of ThreadedCompileGILHolder which is a nop
// in GIL disabled builds.
class FreeThreadedJITEntrypointGuard {
 public:
  FreeThreadedJITEntrypointGuard() {
    if constexpr (kFreeThreadedBuild) {
      freeThreadedJITEntrypointMutex().lock();
    }
  }

  ~FreeThreadedJITEntrypointGuard() {
    if constexpr (kFreeThreadedBuild) {
      freeThreadedJITEntrypointMutex().unlock();
    }
  }

  FreeThreadedJITEntrypointGuard(const FreeThreadedJITEntrypointGuard&) =
      delete;
  FreeThreadedJITEntrypointGuard& operator=(
      const FreeThreadedJITEntrypointGuard&) = delete;
  FreeThreadedJITEntrypointGuard(FreeThreadedJITEntrypointGuard&&) = delete;
  FreeThreadedJITEntrypointGuard& operator=(FreeThreadedJITEntrypointGuard&&) =
      delete;
};

// State handed off to the background compilation worker. Holds Python
// references (via the preloaders, func, and code) that must be released while
// attached to the interpreter.
struct BackgroundCompileTask {
  Ref<PyFunctionObject> func;
  hir::PreloaderMap preloaders;
  Ref<PyCodeObject> code;
  Ref<PyDictObject> builtins;
  Ref<PyDictObject> globals;
};

// Process-wide state for background compilation. A single long-lived worker
// thread (started lazily on the first scheduled compile) consumes `queue`.
// Lives as a member of Context.
struct BackgroundCompileRegistry {
  BackgroundCompileRegistry() = default;

  BackgroundCompileRegistry(const BackgroundCompileRegistry&) = delete;
  BackgroundCompileRegistry& operator=(const BackgroundCompileRegistry&) =
      delete;

  // Guards every field below.
  std::mutex mutex;
  // Notified when work is enqueued or a stop is requested; the worker waits on
  // it.
  std::condition_variable queue_cv;
  // Notified when a compile finishes; finalization waits on it to drain.
  std::condition_variable drain_cv;
  // Pending compilation tasks waiting for the worker.
  std::deque<std::unique_ptr<BackgroundCompileTask>> queue;
  // How many background compiles are scheduled but not yet finished (covers
  // both queued and in-progress tasks).
  size_t in_flight_count{0};
  // The single worker thread, and whether it has been started.
  std::thread worker;
  bool worker_started{false};
  // Set when we want the background compilation thread to shutdown and
  // stop processing further requests
  bool shutdown{false};
};

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
using DeoptStats =
    UnorderedMap<const CodeRuntime*, UnorderedMap<std::size_t, DeoptStat>>;

class Builtins {
 public:
  void init();
  bool isInitialized() const;
  std::optional<std::string> find(PyMethodDef* meth) const;
  std::optional<PyMethodDef*> find(const std::string& name) const;

 private:
  std::atomic<bool> is_initialized_{false};
  std::mutex mtx_;
  UnorderedMap<PyMethodDef*, std::string> cfunc_to_name_;
  UnorderedMap<std::string, PyMethodDef*> name_to_cfunc_;
};

// Lookup key for compiled functions in Context: a code object and the globals
// and builtins dicts it was JIT-compiled with.
struct CompilationKey {
  // These three are borrowed references; the values are kept alive by strong
  // references in the corresponding CodeRuntime.
  PyObject* code;
  PyObject* builtins;
  PyObject* globals;

  explicit CompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{func->func_code},
        builtins{func->func_builtins},
        globals{func->func_globals} {}

  explicit CompilationKey(const CompiledFunction& func)
      : code{func.runtime()->code()},
        builtins{func.runtime()->builtins()},
        globals{func.runtime()->globals()} {}

  CompilationKey(PyObject* code, PyObject* builtins, PyObject* globals)
      : code(code), builtins(builtins), globals(globals) {}

  constexpr bool operator==(const CompilationKey& other) const = default;
};

} // namespace cinderx::jit

template <>
struct std::hash<cinderx::jit::CompilationKey> {
  std::size_t operator()(const cinderx::jit::CompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return cinderx::combineHash(
        hasher(key.code), hasher(key.globals), hasher(key.builtins));
  }
};

namespace cinderx::jit {

struct OwnedCompilationKey {
  Ref<> code;
  Ref<> builtins;
  Ref<> globals;

  explicit OwnedCompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{Ref<>::create(func->func_code)},
        builtins{Ref<>::create(func->func_builtins)},
        globals{Ref<>::create(func->func_globals)} {}

  OwnedCompilationKey(Ref<> code, Ref<> builtins, Ref<> globals)
      : code{std::move(code)},
        builtins{std::move(builtins)},
        globals{std::move(globals)} {}

  bool operator==(const OwnedCompilationKey& other) const = default;
};

} // namespace cinderx::jit

template <>
struct std::hash<cinderx::jit::OwnedCompilationKey> {
  std::size_t operator()(const cinderx::jit::OwnedCompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return cinderx::combineHash(
        hasher(key.code.get()),
        hasher(key.globals.get()),
        hasher(key.builtins.get()));
  }
};

namespace cinderx::jit {

/*
 * A jit::Context encapsulates all the state managed by an instance of the JIT.
 */
class Context : public IJitContext, public CompiledFunctionOwner {
 public:
  Context();

  ~Context() override;

  /*
   * Park a function that was deopted because the JIT was disabled: it comes off
   * its compiled entry point but KEEPS the reference it owns on its compile, so
   * the compile is still there to be rebound when the JIT is re-enabled.
   */
  void addDeoptedFunc(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<CompiledFunction> compiled);

  /*
   * Take a function out of the parked set, returning the compile it still owns
   * a reference to - which the caller is now responsible for - or null if it
   * was not parked.
   *
   * The compile is recorded rather than looked up because it can be forgotten
   * while the function is parked (force_uncompile() during a disabled window),
   * after which lookupFunc() would no longer find it and the reference would
   * have nowhere to go.
   */
  BorrowedRef<CompiledFunction> removeDeoptedFunc(
      BorrowedRef<PyFunctionObject> func);

  /* The compile a parked function is holding, without unparking it. */
  BorrowedRef<CompiledFunction> deoptedCompile(
      BorrowedRef<PyFunctionObject> func);

  /*
   * Give up whatever registration `func` has on `compiled`, whether it is on
   * the compiled entry point or parked by a deopt-all.  This can free the
   * CompiledFunction, so callers must not use it afterwards.
   */
  void releaseFuncRegistration(BorrowedRef<PyFunctionObject> func);

  /*
   * Fully remove all effects of compilation from a function.
   */
  void uncompile(BorrowedRef<PyFunctionObject> func);

  /*
   * Adds a record indicating that the specified function is currently being
   * compiled. This is used to prevent multiple threads from compiling the same
   * function at the same time.
   */
  bool addActiveCompile(const CompilationKey& key);

  /*
   * Indicates that the specified function is no longer being compiled.
   */
  void removeActiveCompile(const CompilationKey& key);

  /*
   * Whether the specified compilation is currently registered as in progress.
   */
  bool hasActiveCompile(const CompilationKey& key);

  /*
   * Creates the CompiledFunction object for a given compilation key.
   * The compiled code can then be shared amongst compatible functions.
   */
  Ref<CompiledFunction> makeCompiledFunction(
      BorrowedRef<PyFunctionObject> func,
      const CompilationKey& key,
      CompiledFunctionData&& compiled_func);

  /*
   * Record per-function metadata for a newly compiled function and set the
   * function's entrypoint.
   */
  void finalizeFunc(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<CompiledFunction> compiled);

  /*
   * Remove the specified code object from the known compiled codes.
   */
  void forgetCode(BorrowedRef<PyFunctionObject> func);

  /*
   * Remove the specified code object from the known compiled codes.
   */
  void forgetCompiledFunction(CompiledFunction& function) override;
  /*
   * Look up the compiled function object for a given Python function object.
   */
  BorrowedRef<CompiledFunction> lookupFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Gets the CompiledFunction for a given code/builtins/globals triplet.
   */
  BorrowedRef<CompiledFunction> lookupCode(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals);

  /*
   * Looks up the CodeRuntime for a given function.
   */
  CodeRuntime* lookupCodeRuntime(BorrowedRef<PyFunctionObject> func) override;

  /*
   * Get the map of all compiled code objects, keyed by their address and also
   * their builtins and globals objects.
   */
  const UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>>&
  compiledCodes() const;

  /*
   * Get a range over all function objects parked by a JIT-disabling deopt,
   * along with the compile each one is still holding.
   */
  const UnorderedMap<
      BorrowedRef<PyFunctionObject>,
      BorrowedRef<CompiledFunction>>&
  deoptedFuncs();

  /*
   * Get the total time spent compiling functions thus far.
   */
  std::chrono::milliseconds totalCompileTime() const;

  /*
   * Adds time to the record of how much time has been spent compiling
   * functions.
   */
  void addCompileTime(std::chrono::nanoseconds time);
  /*
   * Set and hold a reference to the cinderjit Python module.
   */
  void setCinderJitModule(Ref<> mod);

  /*
   * Clear cache of compiled code such that subsequent compilations are always
   * full rather than just re-binding pre-compiled code. Only intended to be
   * used during multithreaded_compile_test.
   */
  void clearForMultithreadedCompileTest();

  /*
   * Callbacks invoked by the runtime when a PyFunctionObject is destroyed.
   */
  void funcDestroyed(BorrowedRef<PyFunctionObject> func);

  /*
   * Release the reference `func` owns on its CompiledFunction, if it has one,
   * and put it back on the interpreter entry point.  Used by the patched
   * function tp_clear to break reference cycles.
   */
  void releaseCompiledFuncRef(BorrowedRef<PyFunctionObject> func);

  /*
   * Put every function that is still running one of this Context's compiles
   * back on the interpreter entry point, releasing the reference it owns.
   *
   * A JIT-compiled function's reference to its CompiledFunction is held
   * logically, and lookupFunc() - i.e. compiled_codes_ - is the only way back
   * to it.  Once this Context is gone nothing can find those references again,
   * so they have to be handed back while it is still intact.
   *
   * Normally a no-op: jit::finalize() deopts every compiled function before
   * dropping the Context.  It's the callers that stand up a Context of their
   * own, such as the runtime tests, that reach here with functions attached.
   */
  void releaseFunctionCompileRefs();

  /*
   * Have `data` hold `compiled` on behalf of a nested function, and have
   * `outer` - the function the nested code was found in - report that reference
   * to the garbage collector.
   *
   * A nested function's compile has to outlive any single instance of that
   * nested function, or creating the next instance recompiles it.  The
   * NestedCompileData bridges that gap because it is keyed by the nested code
   * object, which outlives the instances made from it.
   */
  void addNestedCompile(
      BorrowedRef<PyFunctionObject> outer,
      NestedCompileData& data,
      BorrowedRef<CompiledFunction> compiled);

  /*
   * Drop the compile `data` holds, along with its reporting anchor.
   */
  void clearNestedCompile(NestedCompileData& data);

  /*
   * Stop reporting `data`'s compile from its outer function, without dropping
   * the compile itself.
   */
  void unanchorNestedCompile(NestedCompileData& data);

  /*
   * The function that should report a compile of `code` to the GC: the function
   * the code was found in, falling back to `func` itself.
   */
  BorrowedRef<PyFunctionObject> nestedCompileAnchor(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyFunctionObject> func);

  /*
   * Report the nested compiles `outer` is the anchor for to a GC traversal.
   * Returns non-zero if the visit callback did.
   */
  int traverseNestedCompiles(
      BorrowedRef<PyFunctionObject> outer,
      visitproc visit,
      void* arg);

  /* Release the nested compiles `outer` is the anchor for. */
  void releaseNestedCompiles(BorrowedRef<PyFunctionObject> outer);

  /* Cheap gate so processes with no nested compiles pay nothing per traverse.
   */
  bool hasNestedCompiles() const {
    return !nested_compile_anchors_.empty();
  }

  NestedCompileData* getOrCreateNestedCompileData(
      BorrowedRef<> module_name,
      BorrowedRef<PyCodeObject> code,
      JitEligibility eligibility) override;

  NestedCompileData* findNestedCompileData(BorrowedRef<PyCodeObject> code);

  void eraseNestedCompileData(BorrowedRef<PyCodeObject> code);

  /*
   * Recompute the cached eligibility of every known NestedCompileData entry.
   * Needed when the JIT list changes, as eligibility is only sampled when an
   * entry is created.
   */
  void refreshNestedCompileData();

  // Methods moved from Runtime class

  template <typename... Args>
  CodeRuntime* allocateCodeRuntime(Args&&... args) {
    JITCompilationLock lock;
    return code_runtimes_.allocate(std::forward<Args>(args)...);
  }

  void mlockProfilerDependencies();

  // Find a cache for the indirect static entry point for a function.
  void** findFunctionEntryCache(BorrowedRef<PyCodeObject> code);

  void clearFunctionEntryCache(BorrowedRef<PyCodeObject> code);

  // Checks to see if we already have an entry for indirect static entry point
  bool hasFunctionEntryCache(BorrowedRef<PyCodeObject> code) const;

  // Gets information about the primitive arguments that a function
  // is typed to.  Typed object references are explicitly excluded.
  _PyTypedArgsInfo* findFunctionPrimitiveArgInfo(
      BorrowedRef<PyCodeObject> code);

  // Invoke f with the DeoptStat for the given deopt index if it exists.
  // Returns true if the stat was found and f was called.
  //
  // Note: f runs while deopt_stats_mutex_ is held. If f triggers
  // recordDeopt() or clearDeoptStats(), the non-recursive mutex will
  // deadlock.
  template <typename F>
  bool ifDeoptStat(
      const CodeRuntime* code_runtime,
      std::size_t deopt_idx,
      F&& f) const {
    return withLock(deopt_stats_mutex_, [&]() {
      const DeoptStat* stat = deoptStat(code_runtime, deopt_idx);
      if (stat == nullptr) {
        return false;
      }
      f(*stat);
      return true;
    });
  }

  // Record that a deopt of the given index happened at runtime, with an
  // optional guilty value.
  void recordDeopt(
      CodeRuntime* code_runtime,
      std::size_t idx,
      BorrowedRef<> guilty_value);

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

  // Release any references this Context holds to Python objects.
  void releaseReferences();

#ifdef ENABLE_PREFORK_MODEL
  InlineCacheStorage& inlineCacheStorage(CodeRuntime& code_runtime);
#endif

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

  // Validates that the assumptions a pending type watch was created with still
  // hold. Evaluated with the GIL held in watchPendingTypes(), after a
  // threaded/background compile finishes and before the watch is installed.
  // Must only use GIL-held-safe operations: no ThreadedCompileGILHolder, as
  // the compile context is still active when finalizing on a background
  // worker.
  using TypeWatchValidator = std::function<bool()>;

  // When type is modified or an instance of type has __class__ assigned to,
  // call patcher->maybePatch(new_ty).
  //
  // During threaded/background compilation the actual type watch cannot be
  // installed (it needs the interpreter state), so the watch is deferred to
  // watchPendingTypes(). `validate` re-checks the assumptions the compiled
  // code relies on; if it reports false the type may have changed while the
  // GIL was released, so the patcher is eagerly patched instead of watched.
  void watchType(
      BorrowedRef<PyTypeObject> type,
      TypeDeoptPatcher* patcher,
      TypeWatchValidator validate = nullptr);

  // Stops watching for a specific TypeDeoptPatcher.
  void unwatch(TypeDeoptPatcher* patcher) override;

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

  // Checks to see if we've compiled a code but not yet created a
  // CompiledFunction object.
  bool hasCompletedCompile(const CompilationKey& key);

  // Defers finalization of a function with an already-compiled
  // CompiledFunction during multi-threaded compile. The finalization will
  // be performed in finalizeMultiThreadedCompile.
  //
  // Takes over `func`, an owned reference the caller acquired while it held
  // the GIL.  Referencing the function here instead would poke a refcount with
  // the GIL released and race the interpreter.
  void addDeferredFinalization(
      const CompilationKey& key,
      Ref<PyFunctionObject>&& func);

  void finalizeMultiThreadedCompile();

  // Notifies that a compilation is complete. If we're not in multi-threaded
  // compile the CompiledFunction will immediately be created, otherwise the
  // CompiledFunctionData will be preserved until the multi-threaded compile can
  // finalize things.
  //
  // Takes over `func` as addDeferredFinalization() does, and returns it again
  // if it wasn't needed.
  Ref<PyFunctionObject> codeCompiled(
      CompilationKey& key,
      CompiledFunctionData&& compiled_func,
      Ref<PyFunctionObject>&& func);

  BorrowedRef<> strBuildClass();

  void watchPendingTypes();
  void fixupFunctionEntryCachePostMultiThreadedCompile();

  const hir::Type& typeForCommonConstant(int i) const;

  // Accept a CompiledFunctionData for deferred cleanup.  The data will be
  // kept alive until processDeferredCleanup() determines no thread is
  // executing the associated code.
  void deferCompiledData(
      Ref<> code,
      Ref<> builtins,
      Ref<> globals,
      CompiledFunctionData* data) override;

  // Walk all thread stacks and free deferred CompiledFunctionData entries
  // whose code is no longer executing on any thread.
  void processDeferredCleanup();

  // Map of all code objects to the functions that they were found in.
  // Needed for printing the name of the code object and for preloading.
  UnorderedMap<BorrowedRef<PyCodeObject>, BorrowedRef<PyFunctionObject>>&
  codeOuterFunctions() {
    return code_outer_funcs_;
  }

  // Allocate all CodeRuntimes together so they can be mlocked() without
  // including any other data that happened to be on the same page.
  SlabArena<CodeRuntime> code_runtimes_;

#ifdef ENABLE_PREFORK_MODEL
  ContextInlineCacheStorage inline_cache_storage_;
#endif
  SlabArena<void*> pointer_caches_;

  FunctionEntryCacheMap function_entry_caches_;

  // Background compilation state: a single long-lived worker consuming the
  // queue. Lives as a member of Context so its lifetime is tied to the JIT
  // context.
  BackgroundCompileRegistry background_compile_registry_;

  BackgroundCompileRegistry& backgroundCompileRegistry() {
    return background_compile_registry_;
  }
  const BackgroundCompileRegistry& backgroundCompileRegistry() const {
    return background_compile_registry_;
  }

  template <typename F>
  decltype(auto) withLock(std::mutex& lock, F&& f) const {
    if constexpr (kFreeThreadedBuild) {
      std::lock_guard<std::mutex> guard(lock);
      return f();
    }
    return f();
  }

  std::vector<DeoptMetadata> deopt_metadata_;
  DeoptStats deopt_stats_;
  // Only needed in free-threaded builds; kept unconditional so callers can use
  // kFreeThreadedBuild instead of #ifdefs.
  mutable std::mutex deopt_stats_mutex_;
  mutable std::mutex deferred_compile_data_mutex_;

  // Get the stat object for a given deopt. It will not exist if the deopt has
  // never been hit. Caller must hold deopt_stats_mutex_ in free-threaded
  // builds.
  const DeoptStat* deoptStat(
      const CodeRuntime* code_runtime,
      std::size_t deopt_idx) const;

  GuardFailureCallback guard_failure_callback_;

  Builtins builtins_;

  std::unordered_map<
      BorrowedRef<PyTypeObject>,
      std::unordered_set<TypeDeoptPatcher*>>
      type_deopt_patchers_;

  Ref<> str_build_class_;

  // A type watch deferred during threaded/background compilation, along with
  // the validator for the assumptions the compiled code made about the type.
  struct PendingTypeWatch {
    TypeDeoptPatcher* patcher;
    TypeWatchValidator validate;
  };
  std::unordered_map<BorrowedRef<PyTypeObject>, std::vector<PendingTypeWatch>>
      pending_watches_;

  std::vector<hir::Type> common_constant_types_;

 private:
  /* Deopts a function but doesn't touch deopted_funcs_. */
  bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func);

#ifndef ENABLE_PREFORK_MODEL
  InlineCacheStats getAndClearInlineCacheStats(InlineCacheSite::Kind kind);
#endif

  /*
   * Map of all compiled code objects, keyed by their address and also their
   * builtins and globals objects.
   */
  UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>> compiled_codes_;

  /* Set of which functions were JIT-compiled but have since been deopted.
   *
   * Only includes functions deopted due to being disabled so is empty when
   * the JIT is enabled.
   */
  UnorderedMap<BorrowedRef<PyFunctionObject>, BorrowedRef<CompiledFunction>>
      deopted_funcs_;

  /*
   * Index from an outer function to the nested-compile entries whose references
   * it reports to the garbage collector.  See addNestedCompile() for why that
   * reporting is what keeps the compiles collectable.
   *
   * Holds no references of its own: the single reference to each compile lives
   * on the NestedCompileData, and this only says who is responsible for it.
   */
  UnorderedMap<BorrowedRef<PyFunctionObject>, std::vector<NestedCompileData*>>
      nested_compile_anchors_;

  /*
   * Set of compilations that are currently active, across all threads.
   */
  UnorderedSet<CompilationKey> active_compiles_;

  /*
   * Compilations which have been finished but we haven't created the
   * CompiledFunction objects yet. These are used in the multi-threaded compile
   * case to avoid creating CompiledFunction objects until after all the
   * compiles have completed.
   */
  UnorderedMap<
      CompilationKey,
      std::pair<CompiledFunctionData, Ref<PyFunctionObject>>>
      completed_compiles_;

  /*
   * Functions that need to be finalized with already-existing
   * CompiledFunctions. This happens when lookupCode finds a pre-existing
   * CompiledFunction during multi-threaded compile - we can't call
   * finalizeFunc on a worker thread because it does Python allocations.
   *
   * Stores the compilation key rather than the CompiledFunction that lookupCode
   * returned, so that a compile forgotten between scheduling and finalization
   * simply isn't found rather than being resurrected.
   */
  std::vector<std::pair<Ref<PyFunctionObject>, CompilationKey>>
      deferred_finalizations_;

  /*
   * Code which is being kept alive in case it was in use when
   * clearCache was called. Only intended to be used during
   * multithreaded_compile_test.
   */
  std::vector<Ref<CompiledFunction>> orphaned_compiled_codes_;

  Ref<> cinderjit_module_;

  std::atomic_size_t total_compile_time_ms_;

  // Map of all code objects to the functions that they were found in.
  UnorderedMap<BorrowedRef<PyCodeObject>, BorrowedRef<PyFunctionObject>>
      code_outer_funcs_;

  UnorderedMap<BorrowedRef<PyCodeObject>, std::unique_ptr<NestedCompileData>>
      nested_compile_data_;

  std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>>
      deferred_compiled_data_;
};

// A CompilerContext is like a Context but it also holds a compiler object
// of the consumers choosing.
template <typename T>
class CompilerContext : public Context {
 public:
  T& compiler() {
    return compiler_;
  }

 private:
  T compiler_;
};

// Walk all thread stacks and move entries from `pending` whose code is still
// executing back into `still_active`.  Uses jitFrameGetFunction to correctly
// identify JIT frames across all Python versions.  Defined in frame.cpp
// because it needs access to frame internals that context.cpp cannot include.
void retainActiveDeferredData(
    std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>>& pending,
    std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>>&
        still_active);

/*
 * An AotContext is like the JIT context, but it holds onto state for
 * ahead-of-time compiled functions.
 */
class AotContext {
 public:
  struct FuncState {
    elf::CodeNoteData note;
    BorrowedRef<PyFunctionObject> func;
    std::span<const std::byte> compiled_code;

    vectorcallfunc normalEntry() const {
      return reinterpret_cast<vectorcallfunc>(const_cast<std::byte*>(
          compiled_code.data() + note.normal_entry_offset));
    }
  };

  /*
   * Initialize the context with the handle to the AOT bundle created by
   * dlopen().
   */
  void init(void* bundle_handle);

  /* Clean up the context object. */
  void destroy();

  /*
   * Register a new function whose metadata has been parsed out of the AOT
   * bundle.
   */
  void registerFunc(const elf::Note& note);

  /* Look up the state associated with a given Python function. */
  const FuncState* lookupFuncState(BorrowedRef<PyFunctionObject> func);

 private:
  // The handle to the AOT bundle created by dlopen().
  void* bundle_handle_{nullptr};

  UnorderedMap<std::string, FuncState> funcs_;
};

extern AotContext g_aot_ctx;

// Get the global JIT context. Returns nullptr if the JIT is not initialized.
// This is equivalent to jitCtx() but can be used without depending on pyjit.
Context* getContext();

// Functions are enumerated by walking the GC heap, so this is O(heap size) and
// is only suitable for infrequent operations, not for anything on a call path.
std::vector<Ref<PyFunctionObject>> getCompiledFunctions();

} // namespace cinderx::jit
