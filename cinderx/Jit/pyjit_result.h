// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <fmt/format.h>

namespace jit {

// Status codes for the result of JIT attempts.
enum class Result : int32_t {
  OK,

  // We cannot specialize the input.
  //
  // For example, we cannot generate a specialized tp_init slot if the __init__
  // method of the class is not a function.
  CANNOT_SPECIALIZE,

  // A JIT-list is in use and this function is not on it.
  NOT_ON_JITLIST,

  // Someone tried to compile a function but the JIT is not initialized.
  NOT_INITIALIZED,

  // Function is being scheduled for compilation across multiple threads.
  ALREADY_SCHEDULED,

  // Compilation didn't happen because the JIT is currently paused.
  PAUSED,

  // We are compiling with preload required, but did not find a preloader.
  NO_PRELOADER,

  // We are over the maximum amount of code we are allowed to generate.
  OVER_MAX_CODE_SIZE,

  UNKNOWN_ERROR,

  // The JIT raised a Python exception, like a deferred object failing to be
  // resolved during preloading.
  PYTHON_EXCEPTION = -1,
};

} // namespace jit

inline auto format_as(jit::Result e) {
  return fmt::underlying(e);
}
