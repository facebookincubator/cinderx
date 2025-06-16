// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "cinderx/module_state.h"

namespace cinderx {

int ModuleState::traverse(visitproc visit, void* arg) {
  Py_VISIT(builtin_next_);
  return 0;
}

int ModuleState::clear() {
  sys_clear_caches_.reset();
  builtin_next_.reset();
  return 0;
}

void ModuleState::shutdown() {
  cache_manager_.reset();
  runtime_.reset();
  symbolizer_.reset();
}

static ModuleState* s_cinderx_state;

void setModuleState(ModuleState* state) {
  s_cinderx_state = state;
}

ModuleState* getModuleState() {
  return s_cinderx_state;
}

} // namespace cinderx
