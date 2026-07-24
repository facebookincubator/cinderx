// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compilation_lock.h"

namespace cinderx::jit {

std::recursive_mutex& jitCompilationMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

} // namespace cinderx::jit
