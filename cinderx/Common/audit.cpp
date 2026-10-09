// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/audit.h"

#include "internal/pycore_runtime.h"

#include "cinderx/Common/py-portability.h"

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
  // If the runtime state's layout differs from what we were compiled with we
  // cannot safely do the check below.
  if (!Ci_RuntimeStateLayoutMatches()) {
    return true;
  }
#endif
  _Py_AuditHookEntry* audit_hook_head = runtime->audit_hooks.head;

  // Verify that the hook was actually installed.
  for (_Py_AuditHookEntry* e = audit_hook_head; e != nullptr; e = e->next) {
    if (e->hookCFunction == func && e->userData == userData) {
      return true;
    }
  }

  return false;
}

} // extern "C"
