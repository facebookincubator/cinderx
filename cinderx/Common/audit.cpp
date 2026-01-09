// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/audit.h"

#include "internal/pycore_runtime.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_audit.h"
#endif

extern "C" {

bool installAuditHook(Py_AuditHookFunction func, void* userData) {
  if (PySys_AddAuditHook(func, userData) < 0) {
    return false;
  }

  _PyRuntimeState* runtime = &_PyRuntime;
#if PY_VERSION_HEX >= 0x030E0000
  // If the actual runtime state is a different size than we were compiled with
  // we cannot safely do the check below.
  if (runtime->debug_offsets.runtime_state.size != sizeof(_PyRuntimeState)) {
    return true;
  }
#endif
  _Py_AuditHookEntry* audit_hook_head =
#if PY_VERSION_HEX < 0x030C0000
      runtime->audit_hook_head
#else
      runtime->audit_hooks.head
#endif
      ;

  // Verify that the hook was actually installed.
  for (_Py_AuditHookEntry* e = audit_hook_head; e != nullptr; e = e->next) {
    if (e->hookCFunction == func && e->userData == userData) {
      return true;
    }
  }

  return false;
}

} // extern "C"
