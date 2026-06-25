// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/config.h"

namespace cinderx::jit {

Config s_jit_config;

bool isJitInitialized() {
  return getConfig().state != State::kNotInitialized;
}

bool isJitUsable() {
  return getConfig().state == State::kRunning;
}

bool isJitPaused() {
  return getConfig().state == State::kPaused;
}

} // namespace cinderx::jit
