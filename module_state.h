// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#include "cinderx/Jit/global_cache_iface.h"
#include "cinderx/Jit/runtime_iface.h"
#include "cinderx/Jit/symbolizer_iface.h"

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

  // Gets the value of sys._clear_type_caches when CinderX was initialized.
  BorrowedRef<> sysClearCaches() const {
    return sys_clear_caches_;
  }

 private:
  std::unique_ptr<jit::IGlobalCacheManager> cache_manager_;
  std::unique_ptr<jit::IRuntime> runtime_;
  std::unique_ptr<jit::ISymbolizer> symbolizer_;
  Ref<PyTypeObject> coro_type_, gen_type_;
  Ref<> sys_clear_caches_;
};

void setModuleState(ModuleState* state);
ModuleState* getModuleState();

} // namespace cinderx
