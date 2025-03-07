// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

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
};

void setModuleState(ModuleState* state);
ModuleState* getModuleState();

} // namespace cinderx
