// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus

#include "cinderx/Jit/code_allocator_iface.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/global_cache_iface.h"
#include "cinderx/Jit/runtime_iface.h"
#include "cinderx/Jit/symbolizer_iface.h"
#include "cinderx/async_lazy_value_iface.h"

#include <memory>

namespace cinderx {

class ModuleState {
 public:
  // Implements CPython's traverse functionality for tracing through to GC
  // references
  int traverse(visitproc visit, void* arg);

  // Implements CPython's clear functionality for dropping GC references
  int clear();

  // CPython owns the lifetime of this object so we don't need to worry
  // about deleting it, but we do need a way to cleanup any state it
  // holds onto.
  void shutdown();

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

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
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

  bool initialized() const {
    return initialized_;
  }

  void setInitialized(bool init) {
    initialized_ = init;
  }

 private:
  std::unique_ptr<jit::IGlobalCacheManager> cache_manager_;
  std::unique_ptr<jit::ICodeAllocator> code_allocator_;
  std::unique_ptr<jit::IRuntime> runtime_;
  std::unique_ptr<jit::ISymbolizer> symbolizer_;
  std::unique_ptr<IAsyncLazyValueState> async_lazy_value_;
  Ref<PyTypeObject> coro_type_, gen_type_, anext_awaitable_type_;
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  Ref<> frame_reifier_;
#endif
  Ref<> sys_clear_caches_, builtin_next_;

  // Function objects registered for pre-fork perf-trampoline compilation.
  jit::UnorderedSet<BorrowedRef<PyFunctionObject>> perf_trampoline_worklist_;

  bool initialized_{false};
};

void setModuleState(ModuleState* state);
ModuleState* getModuleState();

} // namespace cinderx

#endif

#ifdef __cplusplus
extern "C" {
#endif

extern vectorcallfunc Ci_PyFunction_Vectorcall;

#ifdef __cplusplus
} // extern "C"
#endif
