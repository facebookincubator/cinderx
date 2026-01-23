// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/watchers.h"
#include "cinderx/Jit/code_allocator_iface.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context_iface.h"
#include "cinderx/Jit/generators_mm_iface.h"
#include "cinderx/Jit/global_cache_iface.h"
#include "cinderx/Jit/jit_list_iface.h"
#include "cinderx/Jit/runtime_iface.h"
#include "cinderx/Jit/symbolizer_iface.h"
#include "cinderx/async_lazy_value_iface.h"

#include <memory>
#include <unordered_map>

namespace cinderx {

class ModuleState {
 public:
  // Implements CPython's traverse functionality for tracing through to GC
  // references
  int traverse(visitproc visit, void* arg);

  // Implements CPython's clear functionality for dropping GC references
  int clear();

  jit::IGlobalCacheManager* cacheManager() const {
    return cache_manager_.get();
  }

  void setCacheManager(jit::IGlobalCacheManager* cache_manager) {
    cache_manager_ = std::unique_ptr<jit::IGlobalCacheManager>(cache_manager);
  }

  jit::ICodeAllocator* codeAllocator() const {
    return code_allocator_.get();
  }

  void setCodeAllocator(jit::ICodeAllocator* code_allocator) {
    code_allocator_.reset(code_allocator);
  }

  jit::IRuntime* runtime() const {
    return runtime_.get();
  }

  void setRuntime(jit::IRuntime* runtime) {
    runtime_ = std::unique_ptr<jit::IRuntime>(runtime);
  }

  jit::IJitContext* jitContext() const {
    return jit_context_.get();
  }

  void setJitContext(jit::IJitContext* context) {
    jit_context_ = std::unique_ptr<jit::IJitContext>(context);
  }

  jit::IJITList* jitList() const {
    return jit_list_.get();
  }

  void setJitList(std::unique_ptr<jit::IJITList> jit_list) {
    jit_list_ = std::move(jit_list);
  }

  jit::ISymbolizer* symbolizer() const {
    return symbolizer_.get();
  }

  void setSymbolizer(jit::ISymbolizer* symbolizer) {
    symbolizer_ = std::unique_ptr<jit::ISymbolizer>(symbolizer);
  }

  IAsyncLazyValueState* asyncLazyValueState() {
    return async_lazy_value_.get();
  }

  void setAsyncLazyValueState(IAsyncLazyValueState* state) {
    async_lazy_value_ = std::unique_ptr<IAsyncLazyValueState>(state);
  }

  void setCoroType(BorrowedRef<PyTypeObject> coro_type) {
    coro_type_ = Ref<PyTypeObject>::create(coro_type);
  }

  BorrowedRef<PyTypeObject> coroType() const {
    return coro_type_;
  }

  void setGenType(BorrowedRef<PyTypeObject> gen_type) {
    gen_type_ = Ref<PyTypeObject>::create(gen_type);
  }

  BorrowedRef<PyTypeObject> genType() const {
    return gen_type_;
  }

  // Sets the value of sys._clear_type_caches when CinderX was initialized.
  // We then replace it with a function which forwards to the original.
  void setSysClearCaches(BorrowedRef<> clear_caches) {
    sys_clear_caches_ = Ref<>::create(clear_caches);
  }

#if PY_VERSION_HEX < 0x030E0000
  void setFrameReifier(BorrowedRef<> frame_reifier) {
    frame_reifier_ = Ref<>::create(frame_reifier);
  }

  BorrowedRef<> frameReifier() const {
    return frame_reifier_;
  }
#endif

  // Gets the value of sys._clear_type_caches when CinderX was initialized.
  BorrowedRef<> sysClearCaches() const {
    return sys_clear_caches_;
  }

  BorrowedRef<> getOriginalSysMonitoringRegisterCallback() const {
    return orig_sys_monitoring_register_callback_;
  }

  void setOriginalSysMonitoringRegisterCallback(BorrowedRef<> func) {
    orig_sys_monitoring_register_callback_ = Ref<>::create(func);
  }

  BorrowedRef<> getOriginalSysSetProfile() const {
    return orig_sys_setprofile_;
  }

  void setOriginalSysSetProfile(BorrowedRef<> func) {
    orig_sys_setprofile_ = Ref<>::create(func);
  }

  BorrowedRef<> getOriginalSysSetTrace() const {
    return orig_sys_settrace_;
  }

  void setOriginalSysSetTrace(BorrowedRef<> func) {
    orig_sys_settrace_ = Ref<>::create(func);
  }

  void setAnextAwaitableType(BorrowedRef<PyTypeObject> type) {
    anext_awaitable_type_ = Ref<PyTypeObject>::create(type);
  }

  BorrowedRef<PyTypeObject> anextAwaitableType() const {
    return anext_awaitable_type_;
  }

  void setBuiltinNext(BorrowedRef<> builtin_next) {
    builtin_next_ = Ref<>::create(builtin_next);
  }

  BorrowedRef<> builtinNext() const {
    return builtin_next_;
  }

  jit::UnorderedSet<BorrowedRef<PyFunctionObject>>& perfTrampolineWorklist() {
    return perf_trampoline_worklist_;
  }

  void setModule(BorrowedRef<> module) {
    cinderx_module_ = module;
  }

  // Returns the PyModule instance for the CinderX module. This can be useful if
  // we have live data backed by the module, in which case we can increase the
  // refcount of the module to prevent it from being freed prematurely.
  BorrowedRef<> module() const {
    return cinderx_module_;
  }

  jit::IJitGenFreeList* jitGenFreeList() const {
    return jit_gen_free_list_.get();
  }

  void setJitGenFreeList(jit::IJitGenFreeList* jit_gen_free_list) {
    jit_gen_free_list_ =
        std::unique_ptr<jit::IJitGenFreeList>(jit_gen_free_list);
  }

  // Returns a dictionary of type->dict[name, members] for standard builtin
  // types.
  std::unordered_map<PyTypeObject*, Ref<>>& builtinMembers() {
    return builtin_members_;
  }

  bool initBuiltinMembers();

  WatcherState& watcherState();

  jit::UnorderedSet<BorrowedRef<>>& registeredCompilationUnits();

  jit::UnorderedMap<BorrowedRef<PyCodeObject>, BorrowedRef<PyFunctionObject>>&
  codeOuterFunctions();

 private:
  WatcherState watcher_state_;

  std::unique_ptr<jit::IGlobalCacheManager> cache_manager_;
  std::unique_ptr<jit::ICodeAllocator> code_allocator_;
  std::unique_ptr<jit::IRuntime> runtime_;
  std::unique_ptr<jit::ISymbolizer> symbolizer_;
  std::unique_ptr<jit::IJitContext> jit_context_;
  std::unique_ptr<jit::IJITList> jit_list_;
  std::unique_ptr<IAsyncLazyValueState> async_lazy_value_;
  std::unique_ptr<jit::IJitGenFreeList> jit_gen_free_list_;
  Ref<PyTypeObject> coro_type_, gen_type_, anext_awaitable_type_;
  std::unordered_map<PyTypeObject*, Ref<>> builtin_members_;
#if PY_VERSION_HEX < 0x030E0000
  Ref<> frame_reifier_;
#endif
  Ref<> sys_clear_caches_, builtin_next_;
  Ref<> orig_sys_monitoring_register_callback_;
  Ref<> orig_sys_setprofile_;
  Ref<> orig_sys_settrace_;

  // Function and code objects ("units") registered for compilation.
  jit::UnorderedSet<BorrowedRef<>> registered_compilation_units;

  // Map of all compiled or to-be-compiled code objects to the functions that
  // they were found in.
  //
  // Needed for printing the name of the code object and for preloading.
  jit::UnorderedMap<BorrowedRef<PyCodeObject>, BorrowedRef<PyFunctionObject>>
      code_outer_funcs;

  // Function objects registered for pre-fork perf-trampoline compilation.
  jit::UnorderedSet<BorrowedRef<PyFunctionObject>> perf_trampoline_worklist_;

  BorrowedRef<> cinderx_module_;
};

// Get the global ModuleState singleton.
ModuleState* getModuleState();

// Get the ModuleState from the CinderX module object.
//
// Prefer this to using the global singleton when possible.
ModuleState* getModuleState(BorrowedRef<> mod);

// Set the global ModuleState singleton, using the CinderX module object.
void setModuleState(BorrowedRef<> mod);

// Unset the global ModuleState singleton, but don't destroy it.
//
// Destroying the state object is done manually.
void removeModuleState();

} // namespace cinderx
