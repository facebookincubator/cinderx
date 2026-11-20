// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/config.h"

#include "cinderx/Common/py-portability.h"

#include <atomic>

namespace cinderx::jit {

Config s_jit_config;

namespace {

// Kept out of Config, which is copied and reset as a whole, so that every
// access is atomic.
std::atomic<State> s_jit_state{State::kNotInitialized};

State loadJitState() {
  return s_jit_state.load(std::memory_order_seq_cst);
}

} // namespace

bool isJitInitialized() {
  return loadJitState() != State::kNotInitialized;
}

bool isJitUsable() {
  return loadJitState() == State::kRunning && !Py_IsFinalizing();
}

bool isJitFinalizing() {
  return loadJitState() == State::kFinalizing || Py_IsFinalizing();
}

bool isJitPaused() {
  return loadJitState() == State::kPaused;
}

void setJitState(State state) {
  s_jit_state.store(state, std::memory_order_seq_cst);
}

} // namespace cinderx::jit
