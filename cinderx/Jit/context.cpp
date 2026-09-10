// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "internal/pycore_interp.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/compilation_lock.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/eligibility.h"
#include "cinderx/Jit/nested_compile.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#ifndef WIN32
#include <dlfcn.h>
#include <sys/mman.h>
#endif

namespace cinderx::jit {

namespace {

PyModuleDef* findBuiltinsModule() {
  // We want to check the exact function address, rather than relying on modules
  // which can be mutated.  First find builtins, which we have to do a search
  // for because PyEval_GetBuiltins() returns the module dict.
  BorrowedRef<> mods =
      CI_INTERP_IMPORT_FIELD(_PyInterpreterState_GET(), modules_by_index);
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(mods); i++) {
    BorrowedRef<> cur = PyList_GET_ITEM(mods.get(), i);
    if (Py_IsNone(cur)) {
      continue;
    }
    PyModuleDef* def = PyModule_GetDef(cur);
    if (def == nullptr) {
      PyErr_Clear();
      continue;
    }
    if (std::strcmp(def->m_name, "builtins") == 0) {
      return def;
    }
  }
  return nullptr;
}

#ifndef ENABLE_PREFORK_MODEL
void collectAndClearInlineCacheStats(
    InlineCacheStats& stats,
    const std::vector<InlineCacheSite>& sites,
    InlineCacheSite::Kind kind) {
  for (const auto& site : sites) {
    if (site.kind != kind) {
      continue;
    }
    auto collect = [&stats](auto* cache) {
      if (cache->cacheStats() == nullptr) {
        return;
      }
      stats.push_back(*cache->cacheStats());
      cache->clearCacheStats();
    };
    if (kind == InlineCacheSite::Kind::kLoadMethod) {
      collect(site.cache.load_method);
    } else {
      JIT_DCHECK(
          kind == InlineCacheSite::Kind::kLoadTypeMethod,
          "unsupported inline cache stats kind");
      collect(site.cache.load_type_method);
    }
  }
}
#endif

} // namespace

AotContext g_aot_ctx;

std::recursive_mutex& freeThreadedJITEntrypointMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point) {
  return yield_point->isYieldFrom()
      ? reinterpret_cast<PyObject*>(
            *(reinterpret_cast<uint64_t*>(gen_footer) +
              yield_point->yieldFromOffset()))
      : nullptr;
}

void Builtins::init() {
  auto guard = std::lock_guard{mtx_};
  if (is_initialized_) {
    return;
  }
  PyModuleDef* builtins = findBuiltinsModule();
  JIT_CHECK(builtins != nullptr, "Could not find builtins module");

  auto add = [this](const std::string& name, PyMethodDef* meth) {
    cfunc_to_name_[meth] = name;
    name_to_cfunc_[name] = meth;
  };
  // Find all free functions.
  for (PyMethodDef* fdef = builtins->m_methods; fdef->ml_name != nullptr;
       fdef++) {
    add(fdef->ml_name, fdef);
  }
  // Find all methods on types.
  PyTypeObject* types[] = {
      &PyDict_Type,
      &PyList_Type,
      &PyTuple_Type,
      &PyUnicode_Type,
  };
  for (auto type : types) {
    for (PyMethodDef* fdef = type->tp_methods; fdef->ml_name != nullptr;
         fdef++) {
      add(fmt::format("{}.{}", type->tp_name, fdef->ml_name), fdef);
    }
  }
  // Only mark as initialized after everything is done to avoid concurrent
  // reads of an unfinished map.
  is_initialized_ = true;
}

bool Builtins::isInitialized() const {
  return is_initialized_;
}

std::optional<std::string> Builtins::find(PyMethodDef* meth) const {
  auto result = cfunc_to_name_.find(meth);
  if (result == cfunc_to_name_.end()) {
    return std::nullopt;
  }
  return result->second;
}

std::optional<PyMethodDef*> Builtins::find(const std::string& name) const {
  auto result = name_to_cfunc_.find(name);
  if (result == name_to_cfunc_.end()) {
    return std::nullopt;
  }
  return result->second;
}

Context::Context() : str_build_class_(Ref<>::create(&_Py_ID(__build_class__))) {
#if PY_VERSION_HEX >= 0x030E0000
  for (int i = 0; i < NUM_COMMON_CONSTANTS; i++) {
    JIT_CHECK(Ci_common_consts[i] != nullptr, "common_consts[{}] is null", i);
    common_constant_types_.emplace_back(
        hir::Type::fromObject(Ci_common_consts[i]));
  }
#endif
}

Context::~Context() {
  // Do this first, while everything the release path re-enters - lookups,
  // forgetCompiledFunction(), the nested-compile maps - is still intact.
  releaseFunctionCompileRefs();

  // Code objects outlive the Context, so their back-pointers into
  // nested_compile_data_ have to be dropped before it's destroyed.
  for (auto& [code, data] : nested_compile_data_) {
    unpublishNestedCompileData(data.get());
  }

  // Clear all of the CompiledFunction's before we clear out the memory used for
  // the CodeRuntime allocated in the slab.
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  for (auto& code : compiled_codes_) {
    code.second->clear(true /* context_finalizing */);
  }

  // Also clear orphaned CFs (from clearForMultithreadedCompileTest) whose
  // data_ still references CodeRuntimes in our slab.
  for (auto& cf : orphaned_compiled_codes_) {
    if (cf->data() != nullptr && cf->data()->runtime != nullptr) {
      cf->data()->runtime = nullptr;
    }
  }

  // The deferred map holds owning references to module dicts (the code's
  // globals/builtins captured in OwnedCompilationKey) as well as the
  // CompiledFunctionData objects.  During interpreter finalization those dicts
  // may already have been GC-untracked by the runtime while our map still holds
  // the last reference.  Letting the map's destructor decref them would re-run
  // PyObject_GC_UnTrack on an already-untracked object and crash.  Since the
  // process is exiting, intentionally leak these references instead of
  // releasing them.  Outside of finalization the normal map destruction
  // releases them.
  if (Py_IsFinalizing()) {
    for (auto& entry : deferred_compiled_data_) {
      OwnedCompilationKey& key = const_cast<OwnedCompilationKey&>(entry.first);
      (void)key.code.release();
      (void)key.builtins.release();
      (void)key.globals.release();
      (void)entry.second.release();
    }
  }
}

void Context::mlockProfilerDependencies() {
#ifndef WIN32
  for (auto& codert : code_runtimes_) {
    if (codert.isCleared()) {
      continue;
    }
    PyCodeObject* code = codert.code().get();
    if (code == nullptr) {
      continue;
    }
    ::mlock(code, sizeof(PyCodeObject));
    ::mlock(code->co_qualname, Py_SIZE(code->co_qualname));
  }
  code_runtimes_.mlock();
#endif
}

Ref<> Context::pageInProfilerDependencies() {
  std::vector<Ref<>> qualname_refs;
  // We want to force the OS to page in the memory on the
  // code_rt->code->qualname path and keep the compiler from optimizing away
  // the code to do so. There are probably more efficient ways of doing this
  // but perf isn't a major concern.
  {
    JITCompilationLock lock;
    for (auto& code_rt : code_runtimes_) {
      if (code_rt.isCleared()) {
        continue;
      }
      BorrowedRef<> qualname = code_rt.code()->co_qualname;
      if (qualname == nullptr) {
        continue;
      }
      qualname_refs.push_back(Ref<>::create(qualname));
    }
  }

  Ref<> qualnames = Ref<>::steal(PyList_New(qualname_refs.size()));
  if (qualnames == nullptr) {
    return nullptr;
  }
  for (Py_ssize_t i = 0; i < qualname_refs.size(); i++) {
    if (PyList_SetItem(qualnames, i, qualname_refs[i].release()) < 0) {
      return nullptr;
    }
  }
  return qualnames;
}

void** Context::findFunctionEntryCache(BorrowedRef<PyCodeObject> code) {
  auto result = function_entry_caches_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(code),
      std::forward_as_tuple());
  if (result.second) {
    result.first->second.ptr = pointer_caches_.allocate();
    // _PyClassLoader_HasPrimitiveArgs doesn't work well in multi-threaded
    // compile in 3.12+ due to access of a dictionary with non-key strings.
    // We fix this up post-compile in the multi-threaded case.
    if (!ThreadedCompileContext::compileRunning() &&
        _PyClassLoader_HasPrimitiveArgs(code)) {
      result.first->second.arg_info = Ref<_PyTypedArgsInfo>::steal(
          _PyClassLoader_GetTypedArgsInfo(code, 1));
    }
  }
  return result.first->second.ptr;
}

void Context::clearFunctionEntryCache(BorrowedRef<PyCodeObject> code) {
  function_entry_caches_.erase(code);
}

NestedCompileData* Context::getOrCreateNestedCompileData(
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code,
    JitEligibility eligibility) {
  JITCompilationLock lock;
  auto [it, inserted] = nested_compile_data_.try_emplace(code, nullptr);
  if (inserted) {
    it->second =
        std::make_unique<NestedCompileData>(module_name, code, eligibility);
    // Make it reachable from the code object itself, so that every path that
    // creates a function from this code can find it.
    publishNestedCompileData(it->second.get());
  } else {
    it->second->setEligibility(eligibility);
  }
  auto outer_it = code_outer_funcs_.find(code);
  if (outer_it != code_outer_funcs_.end() &&
      outer_it->second->func_code == code) {
    it->second->markOwnsCodeOuterFuncEntry();
  }
  return it->second.get();
}

NestedCompileData* Context::findNestedCompileData(
    BorrowedRef<PyCodeObject> code) {
  JITCompilationLock lock;
  auto it = nested_compile_data_.find(code);
  return it == nested_compile_data_.end() ? nullptr : it->second.get();
}

void Context::eraseNestedCompileData(BorrowedRef<PyCodeObject> code) {
  JITCompilationLock lock;
  auto it = nested_compile_data_.find(code);
  if (it == nested_compile_data_.end()) {
    return;
  }
  unpublishNestedCompileData(it->second.get());
  // The anchor index points at the entry, so it has to let go first.
  unanchorNestedCompile(*it->second);
  // Pull the node out and then let the NestedCompileData get freed
  auto node = nested_compile_data_.extract(it);
}

BorrowedRef<PyFunctionObject> Context::nestedCompileAnchor(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyFunctionObject> func) {
  auto it = code_outer_funcs_.find(code);
  return it != code_outer_funcs_.end() ? it->second : func;
}

void Context::refreshNestedCompileData() {
  JITCompilationLock lock;
  for (auto& [code, data] : nested_compile_data_) {
    data->setEligibility(
        getCompilationEligibility(data->moduleName(), data->code()));
  }
}

// See comments in findFunctionEntryCache.
void Context::fixupFunctionEntryCachePostMultiThreadedCompile() {
  for (auto& entry : function_entry_caches_) {
    BorrowedRef<PyCodeObject> code{entry.first};
    if (entry.second.arg_info.get() == nullptr &&
        _PyClassLoader_HasPrimitiveArgs(code)) {
      entry.second.arg_info = Ref<_PyTypedArgsInfo>::steal(
          _PyClassLoader_GetTypedArgsInfo(code, 1));
    }
  }
}

bool Context::hasFunctionEntryCache(BorrowedRef<PyCodeObject> code) const {
  return function_entry_caches_.find(code) != function_entry_caches_.end();
}

_PyTypedArgsInfo* Context::findFunctionPrimitiveArgInfo(
    BorrowedRef<PyCodeObject> code) {
  auto cache = function_entry_caches_.find(code);
  if (cache == function_entry_caches_.end()) {
    return nullptr;
  }
  return cache->second.arg_info.get();
}

void Context::recordDeopt(
    CodeRuntime* code_runtime,
    std::size_t idx,
    BorrowedRef<> guilty_value) {
  withLock(deopt_stats_mutex_, [&]() {
    DeoptStat& stat = deopt_stats_[code_runtime][idx];
    stat.count++;
    if (guilty_value != nullptr) {
      stat.types.recordType(Py_TYPE(guilty_value));
    }
  });
}

const DeoptStat* Context::deoptStat(
    const CodeRuntime* code_runtime,
    std::size_t deopt_idx) const {
  auto map_it = deopt_stats_.find(code_runtime);
  if (map_it == deopt_stats_.end()) {
    return nullptr;
  }
  auto stat_it = map_it->second.find(deopt_idx);
  if (stat_it == map_it->second.end()) {
    return nullptr;
  }
  return &stat_it->second;
}

void Context::clearDeoptStats() {
  withLock(deopt_stats_mutex_, [&]() { deopt_stats_.clear(); });
}

#ifndef ENABLE_PREFORK_MODEL
InlineCacheStats Context::getAndClearInlineCacheStats(
    InlineCacheSite::Kind kind) {
  JITCompilationLock lock;
  InlineCacheStats stats;
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    BorrowedRef<CompiledFunction> compiled = code_rt.compiledFunction();
    if (compiled != nullptr) {
      collectAndClearInlineCacheStats(
          stats, compiled->inlineCacheSites(), kind);
    }
  }
  withLock(deferred_compile_data_mutex_, [&]() {
    for (auto& entry : deferred_compiled_data_) {
      collectAndClearInlineCacheStats(
          stats, entry.second->inline_cache_storage->inlineCacheSites(), kind);
    }
  });
  return stats;
}
#endif

InlineCacheStats Context::getAndClearLoadMethodCacheStats() {
#ifdef ENABLE_PREFORK_MODEL
  return inline_cache_storage_.getAndClearLoadMethodCacheStats();
#else
  return getAndClearInlineCacheStats(InlineCacheSite::Kind::kLoadMethod);
#endif
}

InlineCacheStats Context::getAndClearLoadTypeMethodCacheStats() {
#ifdef ENABLE_PREFORK_MODEL
  return inline_cache_storage_.getAndClearLoadTypeMethodCacheStats();
#else
  return getAndClearInlineCacheStats(InlineCacheSite::Kind::kLoadTypeMethod);
#endif
}

void Context::setGuardFailureCallback(Context::GuardFailureCallback cb) {
  guard_failure_callback_ = cb;
}

void Context::guardFailed(const DeoptMetadata& deopt_meta) {
  if (guard_failure_callback_) {
    guard_failure_callback_(deopt_meta);
  }
}

void Context::clearGuardFailureCallback() {
  guard_failure_callback_ = nullptr;
}

void Context::releaseReferences() {
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    code_rt.releaseReferences();
  }
  type_deopt_patchers_.clear();
}

#ifdef ENABLE_PREFORK_MODEL
InlineCacheStorage& Context::inlineCacheStorage(
    [[maybe_unused]] CodeRuntime& code_runtime) {
  return inline_cache_storage_;
}
#endif

const Builtins& Context::builtins() {
  // Lock-free fast path followed by single-lock slow path during
  // initialization.
  if (!builtins_.isInitialized()) {
    builtins_.init();
  }
  return builtins_;
}

void Context::unwatch(TypeDeoptPatcher* patcher) {
  JITCompilationLock lock;
  type_deopt_patchers_[patcher->type()].erase(patcher);
}

void Context::watchType(
    BorrowedRef<PyTypeObject> type,
    TypeDeoptPatcher* patcher,
    TypeWatchValidator validate) {
  JITCompilationLock lock;
  type_deopt_patchers_[type].emplace(patcher);
  // We require the interpreter state in order to watch types
  if (ThreadedCompileContext::compileRunning()) {
    pending_watches_[type].emplace_back(patcher, std::move(validate));
    return;
  }

  JIT_CHECK(
      cinderx::getModuleState()->watcher_state.watchType(type) == 0,
      "Failed to watch type {}",
      type->tp_name);
}

BorrowedRef<> Context::strBuildClass() {
  return str_build_class_.get();
}

void Context::watchPendingTypes() {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  JITCompilationLock lock;
  for (auto& [type, watches] : pending_watches_) {
    auto it = type_deopt_patchers_.find(type);
    for (auto& watch : watches) {
      // The type may have been modified after the compile checked its
      // assumptions but before the watch could be installed, in which case
      // the watch would never fire for that change. Eagerly deopt instead.
      if (watch.validate && !watch.validate()) {
        if (watch.patcher->isLinked() && !watch.patcher->isPatched()) {
          watch.patcher->patch();
        }
        if (it != type_deopt_patchers_.end()) {
          it->second.erase(watch.patcher);
        }
      }
    }
    bool still_watched =
        it != type_deopt_patchers_.end() && !it->second.empty();
    if (it != type_deopt_patchers_.end() && it->second.empty()) {
      type_deopt_patchers_.erase(it);
    }
    if (!still_watched) {
      continue;
    }
    JIT_CHECK(
        cinderx::getModuleState()->watcher_state.watchType(type) == 0,
        "Failed to watch pending type {}",
        type->tp_name);
  }
  pending_watches_.clear();
}

void Context::notifyTypeModified(
    BorrowedRef<PyTypeObject> lookup_type,
    BorrowedRef<PyTypeObject> new_type) {
  notifyICsTypeChanged(lookup_type);

  JITCompilationLock lock;
  auto it = type_deopt_patchers_.find(lookup_type);
  if (it == type_deopt_patchers_.end()) {
    return;
  }

  std::unordered_set<TypeDeoptPatcher*> remaining_patchers;
  for (TypeDeoptPatcher* patcher : it->second) {
    if (!patcher->maybePatch(new_type)) {
      remaining_patchers.emplace(patcher);
    }
  }

  if (remaining_patchers.empty()) {
    type_deopt_patchers_.erase(it);
    // don't unwatch type; other watchers may still be watching it
  } else {
    it->second = std::move(remaining_patchers);
  }
}

bool Context::hasCompletedCompile(const CompilationKey& key) {
  JITCompilationLock lock;
  return completed_compiles_.contains(key);
}

void Context::addDeferredFinalization(
    const CompilationKey& key,
    Ref<PyFunctionObject>&& func) {
  JITCompilationLock lock;
  deferred_finalizations_.emplace_back(
      Ref<PyFunctionObject>::steal(func.release()), key);
}

void Context::finalizeMultiThreadedCompile() {
  // Destructed outside the lock: decref can block on the GIL, which deadlocks
  // against funcDestroyed().
  decltype(completed_compiles_) completed;
  decltype(deferred_finalizations_) deferred;

  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  {
    FreeThreadedJITEntrypointGuard guard;
    fixupFunctionEntryCachePostMultiThreadedCompile();
    watchPendingTypes();

    for (auto& codes : completed_compiles_) {
      makeCompiledFunction(
          codes.second.second, codes.first, std::move(codes.second.first));
    }
    completed.swap(completed_compiles_);

    for (auto& [func, key] : deferred_finalizations_) {
      // Re-resolve the compile rather than trusting a pointer cached while the
      // GIL was released; the CompiledFunction may have been freed since, in
      // which case it has already erased itself from compiled_codes_ and there
      // is nothing left to attach the function to.
      auto it = compiled_codes_.find(key);
      if (it != compiled_codes_.end() && !isJitCompiled(func)) {
        finalizeFunc(func, it->second);
      }
    }
    deferred.swap(deferred_finalizations_);
  }
}

void Context::finalizeFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  JIT_DCHECK(
      !isJitCompiled(func), "should never finalize an already compiled func");

  // Add the function to the CompiledFunction's set of functions and set it
  // to be compiled.
  compiled->addFunction(func);

  // Cached regardless of whether `func` still carries its code object's name;
  // see Context::codeCompiled.
  if (NestedCompileData* data = nestedCompileData(func->func_code)) {
    addNestedCompile(
        nestedCompileAnchor(func->func_code, func), *data, compiled);
  }
}

Ref<PyFunctionObject> Context::codeCompiled(
    CompilationKey& key,
    CompiledFunctionData&& compiled_func,
    Ref<PyFunctionObject>&& func) {
  addCompileTime(compiled_func.compile_time);

  if (ThreadedCompileContext::compileRunning()) {
    JITCompilationLock lock;
    // Stealing rather than referencing: no refcount is touched, so this is
    // safe with the GIL released.
    completed_compiles_.emplace(
        key,
        std::pair(
            std::move(compiled_func),
            Ref<PyFunctionObject>::steal(func.release())));
    return nullptr;
  }

  makeCompiledFunction(
      BorrowedRef<PyFunctionObject>{func}, key, std::move(compiled_func));
  return std::move(func);
}

const hir::Type& Context::typeForCommonConstant([[maybe_unused]] int i) const {
#if PY_VERSION_HEX >= 0x030E0000
  return common_constant_types_.at(i);
#endif
  JIT_ABORT("Common constants are a feature of 3.14+");
}

void Context::forgetCode(BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  auto it = compiled_codes_.find(CompilationKey{func});
  if (it == compiled_codes_.end()) {
    return;
  }

  // Hold the compile alive across the teardown below: dropping the
  // nested-compile cache's reference can otherwise be the last one.
  Ref<CompiledFunction> compiled = Ref<CompiledFunction>::create(it->second);

  // Drop the nested-compile cache's reference too, otherwise it would keep the
  // compile alive after we've been asked to forget it.
  auto nested_it = nested_compile_data_.find(
      BorrowedRef<PyCodeObject>{
          reinterpret_cast<PyCodeObject*>(it->first.code)});
  if (nested_it != nested_compile_data_.end() &&
      nested_it->second->compiledFunction() == compiled) {
    clearNestedCompile(*nested_it->second);
  }

  compiled->clear();
  compiled_codes_.erase(CompilationKey{func});
}

void Context::forgetCompiledFunction(CompiledFunction& function) {
  // tp_clear() can reach here from GC without going through a guarded
  // top-level JIT entrypoint, so this path has to take a lock.
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  if (function.runtime() != nullptr) {
    auto nested_it = nested_compile_data_.find(function.runtime()->code());
    if (nested_it != nested_compile_data_.end()) {
      if (nested_it->second->compiledFunction() == &function) {
        unanchorNestedCompile(*nested_it->second);
      }
      nested_it->second->clearCompiledFunction(&function);
    }
    compiled_codes_.erase(CompilationKey{function});
  }
}

BorrowedRef<CompiledFunction> Context::lookupFunc(
    BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

CodeRuntime* Context::lookupCodeRuntime(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* compiled = lookupFunc(func);
  if (compiled == nullptr) {
    // For multi-threaded compile tests we clear the compiled codes while the
    // functions may still be running, so fall back to the orphaned compiles,
    // which still know which functions are using them (orphaned_compiled_codes_
    // will be empty outside of these tests))
    for (auto& orphan : orphaned_compiled_codes_) {
      if (orphan->vectorcallEntry() == func->vectorcall) {
        return orphan->runtime();
      }
    }
    return nullptr;
  }
  return compiled->runtime();
}

const UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>>&
Context::compiledCodes() const {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  return compiled_codes_;
}

std::vector<Ref<PyFunctionObject>> getCompiledFunctions() {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");

  std::vector<Ref<PyFunctionObject>> functions;
  auto wrapper = [](PyObject* obj, void* arg) {
    auto funcs = static_cast<std::vector<Ref<PyFunctionObject>>*>(arg);
    if (PyFunction_Check(obj)) {
      BorrowedRef<PyFunctionObject> func{obj};
      if (isJitCompiled(func)) {
        funcs->push_back(Ref<PyFunctionObject>::create(func));
      }
    }
    return 1;
  };
  PyUnstable_GC_VisitObjects(wrapper, static_cast<void*>(&functions));
  return functions;
}

const UnorderedMap<
    BorrowedRef<PyFunctionObject>,
    BorrowedRef<CompiledFunction>>&
Context::deoptedFuncs() {
  return deopted_funcs_;
}

void Context::addCompileTime(std::chrono::nanoseconds time) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time);
  total_compile_time_ms_.fetch_add(ms.count(), std::memory_order_relaxed);
}

std::chrono::milliseconds Context::totalCompileTime() const {
  return std::chrono::milliseconds{
      total_compile_time_ms_.load(std::memory_order_relaxed)};
}

void Context::setCinderJitModule(Ref<> mod) {
  cinderjit_module_ = std::move(mod);
}

void Context::clearForMultithreadedCompileTest() {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  // Visit-safe: a map lookup, a field store, and an incref into a C++ vector.
  // Nothing here allocates or frees a Python object.
  for (auto& func : getCompiledFunctions()) {
    BorrowedRef<CompiledFunction> compiled = lookupFunc(func);
    if (compiled == nullptr) {
      return;
    }
    // Disconnect from Context so clear() on eventual destruction won't call
    // back into us (e.g., forgetCompiledFunction, unwatch).
    compiled->setOwner(nullptr);
    // Keep the old CompiledFunction alive via a strong reference.
    orphaned_compiled_codes_.emplace_back(
        Ref<CompiledFunction>::create(compiled));
    compiled->removeFunction(func);
  }
  compiled_codes_.clear();
  nested_compile_anchors_.clear();
  for (auto& [_, data] : nested_compile_data_) {
    data->setOuterFunc(nullptr);
    data->setCompiledFunction(nullptr);
  }
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  FreeThreadedJITEntrypointGuard guard;
  releaseFuncRegistration(func);
  releaseNestedCompiles(func);
  // This doesn't modify compiled_codes_, so if this is a nested function it can
  // easily be reopted later.
}

void Context::releaseCompiledFuncRef(BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  FreeThreadedJITEntrypointGuard guard;
  JITCompilationLock lock;
  releaseFuncRegistration(func);
}

void Context::releaseFuncRegistration(BorrowedRef<PyFunctionObject> func) {
  if (BorrowedRef<CompiledFunction> parked = removeDeoptedFunc(func)) {
    // Parked by a deopt-all: it still owns a reference to its compile even
    // though its vectorcall no longer says so, and removeFunction() would miss
    // it for exactly that reason.  Released against the compile it was parked
    // on, which is not necessarily the one currently registered for its code.
    parked->releaseDeoptedFunction(func);
    return;
  }
  if (BorrowedRef<CompiledFunction> compiled = lookupFunc(func)) {
    // Puts the function back on the interpreter entry point if it was using
    // this compile, and drops the reference that went with it.
    compiled->removeFunction(func);
  }
}

void Context::releaseFunctionCompileRefs() {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  UnorderedMap<vectorcallfunc, BorrowedRef<CompiledFunction>> by_entry;
  for (auto& [key, compiled] : compiled_codes_) {
    if (compiled->numFunctions() > 0) {
      by_entry.emplace(compiled->vectorcallEntry(), compiled);
    }
  }
  if (by_entry.empty()) {
    return;
  }

  FreeThreadedJITEntrypointGuard guard;
  // removeFunction() can drop the last reference to a compile, which re-enters
  // forgetCompiledFunction() and erases it from compiled_codes_.  That's why
  // the index above is built up front instead of walking compiled_codes_ here.
  for (auto& func : getCompiledFunctions()) {
    auto it = by_entry.find(func->vectorcall);
    if (it != by_entry.end()) {
      it->second->removeFunction(func);
    }
  }
}

BorrowedRef<CompiledFunction> Context::lookupCode(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  // This function should be called with the GIL held but we can't assert it
  // here. We might be called during parallel GC in which case we will have no
  // interpreter state.
  auto it = compiled_codes_.find(CompilationKey{code, builtins, globals});
  return it == compiled_codes_.end() ? nullptr : it->second.get();
}

void Context::addNestedCompile(
    BorrowedRef<PyFunctionObject> outer,
    NestedCompileData& data,
    BorrowedRef<CompiledFunction> compiled) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  FreeThreadedJITEntrypointGuard guard;
  if (outer == nullptr) {
    // With no function to report the reference from, holding the compile would
    // pin its code object and its module's globals for the life of the process.
    // Recompiling later is the lesser evil.
    clearNestedCompile(data);
    return;
  }
  if (outer != data.outerFunc()) {
    // Re-anchoring: the entry is changing hands, so take it off the old
    // anchor's list first.
    unanchorNestedCompile(data);
    nested_compile_anchors_[outer].emplace_back(&data);
    data.setOuterFunc(outer);
  }
  data.setCompiledFunction(compiled);
}

void Context::unanchorNestedCompile(NestedCompileData& data) {
  BorrowedRef<PyFunctionObject> outer = data.outerFunc();
  if (outer == nullptr) {
    return;
  }
  data.setOuterFunc(nullptr);
  auto it = nested_compile_anchors_.find(outer);
  if (it == nested_compile_anchors_.end()) {
    return;
  }
  std::erase(it->second, &data);
  if (it->second.empty()) {
    nested_compile_anchors_.erase(it);
  }
}

void Context::clearNestedCompile(NestedCompileData& data) {
  unanchorNestedCompile(data);
  // May drop the last reference to the compile, which re-enters
  // forgetCompiledFunction(), so nothing above may still be mid-update.
  data.setCompiledFunction(nullptr);
}

int Context::traverseNestedCompiles(
    BorrowedRef<PyFunctionObject> outer,
    visitproc visit,
    void* arg) {
  // No lock: this only runs from a GC traversal, which holds the GIL or has the
  // world stopped.  See lookupCode.
  auto it = nested_compile_anchors_.find(outer);
  if (it == nested_compile_anchors_.end()) {
    return 0;
  }
  for (NestedCompileData* data : it->second) {
    BorrowedRef<CompiledFunction> compiled = data->compiledFunction();
    if (isCollectableCompile(compiled)) {
      Py_VISIT(reinterpret_cast<PyObject*>(compiled.get()));
    }
  }
  return 0;
}

void Context::releaseNestedCompiles(BorrowedRef<PyFunctionObject> outer) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  FreeThreadedJITEntrypointGuard guard;
  auto it = nested_compile_anchors_.find(outer);
  if (it == nested_compile_anchors_.end()) {
    return;
  }
  // Detach before releasing: dropping the last reference to a CompiledFunction
  // re-enters forgetCompiledFunction(), and we must not be holding an iterator
  // into this map when it does.
  std::vector<NestedCompileData*> anchored = std::move(it->second);
  nested_compile_anchors_.erase(it);
  for (NestedCompileData* data : anchored) {
    data->setOuterFunc(nullptr);
    data->setCompiledFunction(nullptr);
  }
}

void Context::addDeoptedFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  JITCompilationLock lock;
  deopted_funcs_.emplace(func, compiled);
}

BorrowedRef<CompiledFunction> Context::removeDeoptedFunc(
    BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  // This is almost always empty and only has things when the JIT is disabled.
  if (deopted_funcs_.empty()) {
    return nullptr;
  }
  auto it = deopted_funcs_.find(func);
  if (it == deopted_funcs_.end()) {
    return nullptr;
  }
  BorrowedRef<CompiledFunction> compiled = it->second;
  deopted_funcs_.erase(it);
  return compiled;
}

BorrowedRef<CompiledFunction> Context::deoptedCompile(
    BorrowedRef<PyFunctionObject> func) {
  if (deopted_funcs_.empty()) {
    return nullptr;
  }
  auto it = deopted_funcs_.find(func);
  return it == deopted_funcs_.end() ? nullptr : it->second;
}

bool Context::addActiveCompile(const CompilationKey& key) {
  JITCompilationLock lock;
  return active_compiles_.insert(key).second;
}

void Context::removeActiveCompile(const CompilationKey& key) {
  JITCompilationLock lock;
  active_compiles_.erase(key);
}

bool Context::hasActiveCompile(const CompilationKey& key) {
  JITCompilationLock lock;
  return active_compiles_.contains(key);
}

Ref<CompiledFunction> Context::makeCompiledFunction(
    BorrowedRef<PyFunctionObject> func,
    const CompilationKey& key,
    CompiledFunctionData&& compiled_func) {
  JIT_DCHECK(PyThreadState_GetUnchecked() != nullptr, "GIL should be held");
  BorrowedRef<PyFunctionObject> outer = nullptr;
  auto outer_it = code_outer_funcs_.find(key.code);
  if (outer_it != code_outer_funcs_.end() && outer_it->second != func) {
    outer = outer_it->second;
  }
  // In the prefork model JIT-compiled functions are always immortalized (see
  // kPreforkModel); otherwise they're only immortal if the owning function is.
  bool immortal = kPreforkModel || (func != nullptr && _Py_IsImmortal(func)) ||
      (outer != nullptr && _Py_IsImmortal(outer));
  auto compiled = CompiledFunction::create(std::move(compiled_func), immortal);
  if (compiled == nullptr) {
    return nullptr;
  }

  compiled->setOwner(this);

  BorrowedRef<PyCodeObject> code{key.code};
  if (hasFunctionEntryCache(code)) {
    void** indirect = findFunctionEntryCache(code);
    *indirect = compiled->staticEntry();
  }

  if (compiled->runtime() != nullptr) {
    compiled->runtime()->setCompiledFunction(compiled);
  }

  if (func != nullptr) {
    finalizeFunc(func, compiled);
  }

  // Borrowed; the compile is kept alive by the function objects using it.
  auto pair = compiled_codes_.emplace(key, compiled);
  JIT_CHECK(
      pair.second,
      "CompilationKey already present {}",
      PyUnicode_AsUTF8(reinterpret_cast<PyCodeObject*>(key.code)->co_qualname));
  // The nested-compile entry is what carries the compile across the gaps
  // between instances of a nested function, anchored on the function the code
  // was found in.  The compile is cached even when `func` has been renamed away
  // from its code object -- functools.update_wrapper() renames every instance
  // of a nested function, and without this each one recompiles from scratch.
  // The name still decides who may *take* the compile: scheduleNestedFunction()
  // only speaks for functions carrying the code's own name, and checks the
  // entry's eligibility before handing the compile over.
  auto nested_it = nested_compile_data_.find(key.code);
  if (nested_it != nested_compile_data_.end()) {
    addNestedCompile(
        nestedCompileAnchor(key.code, func), *nested_it->second, compiled);
  }
  return compiled;
}

void Context::deferCompiledData(
    Ref<> code,
    Ref<> builtins,
    Ref<> globals,
    CompiledFunctionData* data) {
  OwnedCompilationKey key{
      std::move(code), std::move(builtins), std::move(globals)};
  withLock(deferred_compile_data_mutex_, [&]() {
    deferred_compiled_data_.emplace(
        std::move(key), Ref<CompiledFunctionData>::steal(data));
  });
}

void Context::processDeferredCleanup() {
  // Move the deferred map into a local.  retainActiveDeferredData walks
  // all thread stacks (using jitFrameGetFunction for correct JIT frame
  // access) and moves entries that are still executing back into the
  // member map.  When the local destructs the remaining entries are freed.
  withLock(deferred_compile_data_mutex_, [&]() {
    if (deferred_compiled_data_.empty()) {
      return;
    }

    std::unordered_map<OwnedCompilationKey, Ref<CompiledFunctionData>> pending =
        std::move(deferred_compiled_data_);
    retainActiveDeferredData(pending, deferred_compiled_data_);
  });
}

#ifndef WIN32
void AotContext::init(void* bundle_handle) {
  JIT_CHECK(
      bundle_handle_ == nullptr,
      "Trying to register AOT bundle at {} but already have one at {}",
      bundle_handle,
      bundle_handle_);
  bundle_handle_ = bundle_handle;
}

void AotContext::destroy() {
  if (bundle_handle_ == nullptr) {
    return;
  }

  // TASK(T183003853): Unmap compiled functions and empty out private data
  // structures.

  dlclose(bundle_handle_);
  bundle_handle_ = nullptr;
}

void AotContext::registerFunc(const elf::Note& note) {
  elf::CodeNoteData note_data = elf::parseCodeNote(note);
  JIT_LOG("  Function {}", note.name);
  JIT_LOG("    File: {}", note_data.file_name);
  JIT_LOG("    Line: {}", note_data.lineno);
  JIT_LOG("    Hash: {:#x}", note_data.hash);
  JIT_LOG("    Size: {}", note_data.size);
  JIT_LOG("    Normal Entry: +{:#x}", note_data.normal_entry_offset);
  JIT_LOG(
      "    Static Entry: {}",
      note_data.static_entry_offset
          ? fmt::format("+{:#x}", *note_data.static_entry_offset)
          : "");

  // This could use std::piecewise_construct for better efficiency.
  auto [it, inserted] = funcs_.emplace(note.name, FuncState{});
  JIT_CHECK(inserted, "Duplicate ELF note for function '{}'", note.name);
  it->second.note = std::move(note_data);

  // Compute the compiled function's address after dynamic linking.
  void* address = dlsym(bundle_handle_, note.name.c_str());
  JIT_CHECK(
      address != nullptr,
      "Cannot find AOT-compiled function with name '{}' despite successfully "
      "loading the AOT bundle",
      note.name);
  it->second.compiled_code = {
      reinterpret_cast<const std::byte*>(address), it->second.note.size};
  JIT_LOG("    Address: {}", address);
}

const AotContext::FuncState* AotContext::lookupFuncState(
    BorrowedRef<PyFunctionObject> func) {
  std::string name = funcFullname(func);
  auto it = funcs_.find(name);
  return it != funcs_.end() ? &it->second : nullptr;
}
#endif

Context* getContext() {
  auto state = cinderx::getModuleState();
  if (state == nullptr) {
    return nullptr;
  }
  return static_cast<Context*>(state->jit_context.get());
}

} // namespace cinderx::jit
