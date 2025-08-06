// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "cinderx/module_state.h"

#include "cinderx/Common/log.h"

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

static ModuleState* s_cinderx_state;

void ModuleState::shutdown() {
  cache_manager_.reset();
  runtime_.reset();
  symbolizer_.reset();
  JIT_DCHECK(
      this == s_cinderx_state,
      "Global module state pointer inconsistent with this module state {} != "
      "{}",
      reinterpret_cast<void*>(this),
      reinterpret_cast<void*>(s_cinderx_state));
  s_cinderx_state = nullptr;
}

void setModule(PyObject* m) {
  auto state = reinterpret_cast<cinderx::ModuleState*>(PyModule_GetState(m));
  s_cinderx_state = state;
  state->setModule(m);
}

ModuleState* getModuleState() {
  return s_cinderx_state;
}

} // namespace cinderx

extern "C" {
vectorcallfunc Ci_PyFunction_Vectorcall;
}
