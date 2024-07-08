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
  // the count, and there may be new fields for this. T194019251
  CHANGED_PYCODEOBJECT,
  // Missing f_code, f_lasti, f_gen, f_stackdepth, f_valuestack
  // No longer a frame on PyThreadState. T194018580
  CHANGED_PYFRAMEOBJECT,
  // Objects/dict-common.h is no longer available so by default PyDictValues is
  // now an opaque type. T194021668
  CHANGED_PYDICT,
  // No _jit_data field on generators. T194022335
  GENERATOR_JIT_SUPPORT,  
  SHADOW_FRAMES, // T194018580  
  TSTATE_FROM_RUNTIME, // T194018580  
  REF_TOTAL_CHANGED, // T194027565
  AWAITED_FLAG, // T194027914
  EXCEPTION_HANDLING, // T194028347
  PYCODEUNIT_NOT_AN_INT, // T194019251
  IMMORTALIZATION_DIFFERENT, // T194028563
  MISSING_SUPPRESS_JIT_FLAG, // T194028831
  AUDIT_API_CHANGED,
  // Missing tstate->frame. T194018580
  FRAME_HANDLING_CHANGED, 
  MISSING_VECTORCALL_ARGUMENT_MASK, // T194027914
  INCOMPLETE_PY_AWAITER, // T192550846
  AST_UPDATES, // T194029115
  MISSING_CO_NOFREE, // T194029115
  // Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED, Ci_Py_VECTORCALL_INVOKED_METHOD. T194028831
  NEED_STATIC_FLAGS,
  PYLONG_DATA_CHANGED, // T194029303
  MISSING_PY_TYPEFLAGS_FROZEN, // T194029468
  // This is a macro and used from C code so a bit tricky to stub.
  MISSING_PyHeapType_GET_MEMBERS, // T194021668
  PROFILING_CHANGED, // T194029734
  HOW_TO_DETECT_JIT_CODE_RUNNING, // T194018580
  EXPORT_JIT_OFFSETS_FOR_STROBELIGHT, // T192550846
};

#endif // PY_VERSION_HEX >= 0x030C0000
