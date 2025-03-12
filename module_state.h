// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#include "cinderx/Jit/global_cache_iface.h"
#include "cinderx/Jit/runtime_iface.h"

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

 private:
  std::unique_ptr<jit::IGlobalCacheManager> cache_manager_;
  std::unique_ptr<jit::IRuntime> runtime_;
};

void setModuleState(ModuleState* state);
ModuleState* getModuleState();

} // namespace cinderx
