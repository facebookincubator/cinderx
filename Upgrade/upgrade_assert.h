// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>
#if PY_VERSION_HEX >= 0x030C0000

#include <stdlib.h>
#include <stdio.h>

#define UPGRADE_ASSERT(tag) \
  { \
    fprintf(stderr, "UPGRADE_ASSERT %s @ %d: %s\n", __FILE__, __LINE__, #tag); \
    abort(); \
  }

// Essentially structured comments for code which needs upgrading. This should
// be used sparingly as overuse will lead to hard to debug crashes.
#define UPGRADE_NOTE(tag, task)

enum {
  // Missing co_code, co_varnames, co_freevars, co_cellvars, co_cell2arg
  // Looks like many instances of co_freevars/cellvars are just checking
  // the count, and there may be new fields for this.
  CHANGED_PYCODEOBJECT,
  // Missing f_code, f_lasti, f_gen, f_stackdepth, f_valuestack
  // No longer a frame on PyThreadState
  CHANGED_PYFRAMEOBJECT,
  // Objects/dict-common.h is no longer available so by default PyDictValues is
  // now an opaque type.
  CHANGED_PYDICT,
  // No _jit_data field on generators
  GENERATOR_JIT_SUPPORT,
  SHADOW_FRAMES,
  TSTATE_FROM_RUNTIME,
  REF_TOTAL_CHANGED,
  AWAITED_FLAG,
  EXCEPTION_HANDLING,
  PYCODEUNIT_NOT_AN_INT,
  IMMORTALIZATION_DIFFERENT,
  MISSING_SUPPRESS_JIT_FLAG,
  AUDIT_API_CHANGED,
  // Missing tstate->frame
  FRAME_HANDLING_CHANGED,
  MISSING_VECTORCALL_ARGUMENT_MASK,
  INCOMPLETE_PY_AWAITER,
  AST_UPDATES,
  MISSING_CO_NOFREE,
  // Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED, Ci_Py_VECTORCALL_INVOKED_METHOD
  NEED_STATIC_FLAGS,
  PYLONG_DATA_CHANGED,
  MISSING_PY_TYPEFLAGS_FROZEN,
  // This is a macro and used from C code so a bit tricky to stub.
  MISSING_PyHeapType_GET_MEMBERS,
  PROFILING_CHANGED,
  HOW_TO_DETECT_JIT_CODE_RUNNING,
  EXPORT_JIT_OFFSETS_FOR_STROBELIGHT,
};

#endif // PY_VERSION_HEX >= 0x030C0000
