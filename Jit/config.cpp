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

bool isJitUsable() {
  return getConfig().is_enabled &&
      getConfig().init_state == InitState::kInitialized;
}

} // namespace jit
