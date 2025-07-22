// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wraps PySys_AddAuditHook().
//
// PySys_AddAuditHook() can fail to add the hook but still return 0 if an
// existing audit function aborts the sys.addaudithook event. Since we rely
// on it for correctness, walk the linked list of audit functions and make
// sure ours is there.
bool installAuditHook(Py_AuditHookFunction func, void* userData);

#ifdef __cplusplus
}
#endif
