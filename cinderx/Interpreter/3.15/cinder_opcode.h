// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#define _PyOpcode_num_popped _CiOpcode_num_popped
#define _PyOpcode_num_pushed _CiOpcode_num_pushed
#define _PyOpcode_opcode_metadata _CiOpcode_opcode_metadata
#define _PyOpcode_macro_expansion _CiOpcode_macro_expansion
#define _PyOpcode_OpName _CiOpcode_OpName
#define _PyOpcode_Caches _CiOpcode_Caches
#define _PyOpcode_Deopt _CiOpcode_Deopt
#define _PyOpcode_PseudoTargets _CiOpcode_PseudoTargets

// CPython 3.15 declares the opcode tables with PyAPI_DATA, which expands
// to __declspec(dllimport) on Windows. The renamed _CiOpcode_* tables are
// defined by cinder_opcode.c in this same extension, not imported from
// python3xx.dll, so declare them as plain externs; a dllimport reference
// cannot bind to the static definition and the Windows link fails.
#pragma push_macro("PyAPI_DATA")
#undef PyAPI_DATA
#define PyAPI_DATA(RTYPE) extern RTYPE
#include "internal/pycore_opcode_metadata.h"
#pragma pop_macro("PyAPI_DATA")

#ifndef Ci_INTERNAL_OPCODE_H
#define Ci_INTERNAL_OPCODE_H

#ifndef EXTENDED_OPCODE
#define EXTENDED_OPCODE 126
#endif

#ifndef EAGER_IMPORT_NAME
#define EAGER_IMPORT_NAME 121
#endif

#include "cinderx/Interpreter/cinder_opcode_ids.h"

#endif
