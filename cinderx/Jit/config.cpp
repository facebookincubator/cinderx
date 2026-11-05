// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/config.h"

#include "cinderx/Common/py-portability.h"

#include <atomic>

namespace cinderx::jit {

Config s_jit_config;

bool isJitInitialized() {
  return std::atomic_ref{getMutableConfig().state}.load() !=
      State::kNotInitialized;
}

bool isJitUsable() {
  return std::atomic_ref{getMutableConfig().state}.load() == State::kRunning &&
      !Py_IsFinalizing();
}

bool isJitFinalizing() {
  return std::atomic_ref{getMutableConfig().state}.load() ==
      State::kFinalizing ||
      Py_IsFinalizing();
}

bool isJitPaused() {
  return std::atomic_ref{getMutableConfig().state}.load() == State::kPaused;
}

} // namespace cinderx::jit
