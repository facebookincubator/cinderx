// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#ifndef Ci_INTERNAL_OPCODE_H
#define Ci_INTERNAL_OPCODE_H

#include "cinderx/Interpreter/cinder_opcode_ids.h"


#define _PyOpcode_Caches _CiOpcode_Caches
#define _PyOpcode_Deopt _CiOpcode_Deopt
#define _PyOpcode_OpName _CiOpcode_OpName
#define _PyOpcode_Jump _CiOpcode_Jump


// We are effectively importing two pycore_opcode's here. First we
// import the one generated with CinderX opcodes but with the names
// re-defined to by the _Ci prefixes. Then we import the normal CPython
// one to get the original names as external references.
#ifdef Py_INTERNAL_OPCODE_H
#error Should be included before internal/pycore_opcode.h
#endif


#ifdef EXTRA_CASES
#undef EXTRA_CASES
#endif

// Import the renamed ones with CinderX opcodes
#include "cinderx/Interpreter/cinder_opcode_metadata.h"

#undef _PyOpcode_Caches
#undef _PyOpcode_Deopt
#undef _PyOpcode_OpName
#undef _PyOpcode_Jump

// Bring in the references to the original versions
#ifdef NEED_OPCODE_TABLES
#undef NEED_OPCODE_TABLES
#endif
#undef Py_INTERNAL_OPCODE_H
#include "internal/pycore_opcode.h"

#endif
