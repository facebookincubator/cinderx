// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <mutex>

namespace cinderx::jit {

// Dedicated lock for protecting JIT compilation data structures such as
// compiled_funcs_, compiled_codes_, active_compiles_, completed_compiles_, etc.
// This is a simple mutex, unlike ThreadedCompileSerialize which has additional
// logic for GIL handling during threaded compiles.
std::recursive_mutex& jitCompilationMutex();

// Uses to track if the current thread holds the lock for assertion purposes.
inline thread_local int jitCompilationLockDepth = 0;

class JITCompilationLock {
 public:
  JITCompilationLock() {
    jitCompilationMutex().lock();
    ++jitCompilationLockDepth;
  }

  ~JITCompilationLock() {
    --jitCompilationLockDepth;
    jitCompilationMutex().unlock();
  }

  // Checks if the current thread holds the lock, should only be
  // used for assertions.
  static bool isHeld() {
    return jitCompilationLockDepth > 0;
  }

  JITCompilationLock(const JITCompilationLock&) = delete;
  JITCompilationLock& operator=(const JITCompilationLock&) = delete;
  JITCompilationLock(JITCompilationLock&&) = delete;
  JITCompilationLock& operator=(JITCompilationLock&&) = delete;
};

} // namespace cinderx::jit
