// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/config.h"

namespace jit {

namespace {

Config s_config;

} // namespace

const Config& getConfig() {
  return s_config;
}

Config& getMutableConfig() {
  return s_config;
}

bool isJitInitialized() {
  return getConfig().state != State::kNotInitialized;
}

bool isJitUsable() {
  return getConfig().state == State::kRunning;
}

bool isJitPaused() {
  return getConfig().state == State::kPaused;
}

} // namespace jit
