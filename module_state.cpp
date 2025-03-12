// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "cinderx/module_state.h"

namespace cinderx {

int ModuleState::traverse(visitproc, void*) {
  return 0;
}

int ModuleState::clear() {
  return 0;
}

void ModuleState::shutdown() {
  cache_manager_.reset();
  runtime_.reset();
}

static ModuleState* s_cinderx_state;

void setModuleState(ModuleState* state) {
  s_cinderx_state = state;
}

ModuleState* getModuleState() {
  return s_cinderx_state;
}

} // namespace cinderx
