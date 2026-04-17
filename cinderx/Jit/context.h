// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context_iface.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/elf/note.h"
#include "cinderx/Jit/fixed_type_profiler.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/pyjit_result.h"
#include "cinderx/Jit/type_deopt_patchers.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit {

#ifdef Py_GIL_DISABLED
std::recursive_mutex& freeThreadedJITEntrypointMutex();
#endif

// Free-threaded builds can enter top-level JIT operations concurrently:
// function/code registration, compilation, and destruction hooks.
// Use a dedicated lock instead of ThreadedCompileSerialize, which is acquired
// deeper in the threaded-compile internals.
class FreeThreadedJITEntrypointGuard {
 public:
  FreeThreadedJITEntrypointGuard() {
#ifdef Py_GIL_DISABLED
    freeThreadedJITEntrypointMutex().lock();
#endif
  }

  ~FreeThreadedJITEntrypointGuard() {
#ifdef Py_GIL_DISABLED
    freeThreadedJITEntrypointMutex().unlock();
#endif
  }

  FreeThreadedJITEntrypointGuard(const FreeThreadedJITEntrypointGuard&) =
      delete;
  FreeThreadedJITEntrypointGuard& operator=(
      const FreeThreadedJITEntrypointGuard&) = delete;
  FreeThreadedJITEntrypointGuard(FreeThreadedJITEntrypointGuard&&) = delete;
  FreeThreadedJITEntrypointGuard& operator=(FreeThreadedJITEntrypointGuard&&) =
      delete;
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

// Lookup key for compiled functions in Context: a code object and the globals
// and builtins dicts it was JIT-compiled with.
struct CompilationKey {
  // These three are borrowed references; the values are kept alive by strong
  // references in the corresponding jit::CodeRuntime.
  PyObject* code;
  PyObject* builtins;
  PyObject* globals;

  explicit CompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{func->func_code},
        builtins{func->func_builtins},
        globals{func->func_globals} {}

  explicit CompilationKey(const CompiledFunction& func)
      : code{func.runtime()->frameState()->code()},
        builtins{func.runtime()->frameState()->builtins()},
        globals{func.runtime()->frameState()->globals()} {}

  CompilationKey(PyObject* code, PyObject* builtins, PyObject* globals)
      : code(code), builtins(builtins), globals(globals) {}

  constexpr bool operator==(const CompilationKey& other) const = default;
};

} // namespace jit

template <>
struct std::hash<jit::CompilationKey> {
  std::size_t operator()(const jit::CompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return jit::combineHash(
        hasher(key.code), hasher(key.globals), hasher(key.builtins));
  }
};

namespace jit {

/*
 * A jit::Context encapsulates all the state managed by an instance of the JIT.
 */
class Context : public IJitContext, public CompiledFunctionOwner {
 public:
  Context();

  ~Context() override;

  /*
   * Adds a function to the list of deopted functions - this means the function
   * was once compiled but has now been turned back into a normal Python
   * function. If the JIT is re-enabled the function can be re-initialized to
   * the JITed form.
   */
  void addDeoptedFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Removes a function from the deopted functions set.
   */
  void removeDeoptedFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Fully remove all effects of compilation from a function.
   */
  void uncompile(BorrowedRef<PyFunctionObject> func);

  /*
   * Adds a record indicating that the specified function is currently being
   * compiled. This is used to prevent multiple threads from compiling the same
   * function at the same time.
   */
  bool addActiveCompile(CompilationKey& key);

  /*
   * Indicates that the specified function is no longer being compiled.
   */
  void removeActiveCompile(CompilationKey& key);

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
  bool finalizeFunc(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<CompiledFunction> compiled);

  /*
   * Adds a compiled function to the Context. Returns false if the function was
   * previously added.
   */
  bool addCompiledFunc(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<CompiledFunction> compiled);

  /*
   * Removes a function from the set of functions that are known to be compiled.
   * This happens if a function is deopted.
   *
   * Returns true if the function was removed.
   */
  bool removeCompiledFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Return whether or not this context compiled the supplied function.
   */
  bool didCompile(BorrowedRef<PyFunctionObject> func);

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
   * Get a range over all function objects that have been compiled.
   */
  const UnorderedMap<
      BorrowedRef<PyFunctionObject>,
      BorrowedRef<CompiledFunction>>&
  compiledFuncs();

  /*
   * Get a range over all function objects that have been compiled and since
   * deopted.
   */
  const UnorderedSet<BorrowedRef<PyFunctionObject>>& deoptedFuncs();

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

  // Methods moved from Runtime class

  template <typename... Args>
  CodeRuntime* allocateCodeRuntime(Args&&... args) {
    return code_runtimes_.allocate(std::forward<Args>(args)...);
  }

  void mlockProfilerDependencies();

  // Find a cache for the indirect static entry point for a function.
  void** findFunctionEntryCache(PyFunctionObject* function);

  void clearFunctionEntryCache(BorrowedRef<PyFunctionObject> function);

  // Checks to see if we already have an entry for indirect static entry point
  bool hasFunctionEntryCache(PyFunctionObject* function) const;

  // Gets information about the primitive arguments that a function
  // is typed to.  Typed object references are explicitly excluded.
  _PyTypedArgsInfo* findFunctionPrimitiveArgInfo(PyFunctionObject* function);

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
#ifdef Py_GIL_DISABLED
    std::lock_guard<std::mutex> lock(deopt_stats_mutex_);
#endif
    const DeoptStat* stat = deoptStat(code_runtime, deopt_idx);
    if (stat == nullptr) {
      return false;
    }
    f(*stat);
    return true;
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

  // Ensure that this Context owns a reference to the given borrowed object,
  // keeping it alive for use by the compiled code. Make CodeRuntime a new
  // owner of the object.
  void addReference(BorrowedRef<> obj);

  // Release any references this Context holds to Python objects.
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
  bool hasCompletedCompile(CompilationKey& key);

  // Defers finalization of a function with an already-compiled
  // CompiledFunction during multi-threaded compile. The finalization will
  // be performed in finalizeMultiThreadedCompile.
  void addDeferredFinalization(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<CompiledFunction> compiled);

  void finalizeMultiThreadedCompile();

  // Notifies that a compilation is complete. If we're not in multi-threaded
  // compile the CompiledFunction will immediately be created, otherwise the
  // CompiledFunctionData will be preserved until the multi-threaded compile can
  // finalize things.
  void codeCompiled(
      BorrowedRef<PyFunctionObject> func,
      CompilationKey& key,
      CompiledFunctionData&& compiled_func);

  BorrowedRef<> zero() override;
  BorrowedRef<> strBuildClass();

  void watchPendingTypes();
  void fixupFunctionEntryCachePostMultiThreadedCompile();

  const hir::Type& typeForCommonConstant(int i) const;

  // Map of all code objects to the functions that they were found in.
  // Needed for printing the name of the code object and for preloading.
  UnorderedMap<BorrowedRef<PyCodeObject>, BorrowedRef<PyFunctionObject>>&
  codeOuterFunctions() {
    return code_outer_funcs_;
  }

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
#ifdef Py_GIL_DISABLED
  mutable std::mutex deopt_stats_mutex_;
#endif

  // Get the stat object for a given deopt.  It will not exist if the deopt has
  // never been hit.  Caller must hold deopt_stats_mutex_ when Py_GIL_DISABLED.
  const DeoptStat* deoptStat(
      const CodeRuntime* code_runtime,
      std::size_t deopt_idx) const;

  GuardFailureCallback guard_failure_callback_;

  // References to Python objects held by this Context
  std::unordered_set<ThreadedRef<PyObject>> references_;
  Builtins builtins_;

  std::unordered_map<
      BorrowedRef<PyTypeObject>,
      std::unordered_set<TypeDeoptPatcher*>>
      type_deopt_patchers_;

  Ref<> zero_;
  Ref<> str_build_class_;
  std::unordered_set<BorrowedRef<PyTypeObject>> pending_watches_;

  std::vector<hir::Type> common_constant_types_;

 private:
  /* Deopts a function but doesn't touch deopted_funcs_. */
  bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func);

  /*
   * Map of all compiled code objects, keyed by their address and also their
   * builtins and globals objects.
   */
  UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>> compiled_codes_;

  /* Set of which functions have JIT-compiled entrypoints. */
  UnorderedMap<BorrowedRef<PyFunctionObject>, BorrowedRef<CompiledFunction>>
      compiled_funcs_;

  /* Set of which functions were JIT-compiled but have since been deopted. */
  UnorderedSet<BorrowedRef<PyFunctionObject>> deopted_funcs_;

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
      std::pair<CompiledFunctionData, ThreadedRef<PyFunctionObject>>>
      completed_compiles_;

  /*
   * Functions that need to be finalized with already-existing
   * CompiledFunctions. This happens when lookupCode finds a pre-existing
   * CompiledFunction during multi-threaded compile - we can't call
   * finalizeFunc on a worker thread because it does Python allocations.
   */
  std::vector<
      std::pair<ThreadedRef<PyFunctionObject>, BorrowedRef<CompiledFunction>>>
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

  jit::UnorderedMap<std::string, FuncState> funcs_;
};

extern AotContext g_aot_ctx;

// Get the global JIT context. Returns nullptr if the JIT is not initialized.
// This is equivalent to jitCtx() but can be used without depending on pyjit.
Context* getContext();

} // namespace jit
