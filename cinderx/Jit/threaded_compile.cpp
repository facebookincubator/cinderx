// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/threaded_compile.h"

namespace jit {

namespace {

ThreadedCompileContext s_threaded_compile_context;

} // namespace

ThreadedCompileContext& getThreadedCompileContext() {
  return s_threaded_compile_context;
}

} // namespace jit
