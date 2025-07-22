// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#ifndef Ci_INTERNAL_OPCODE_H
#define Ci_INTERNAL_OPCODE_H

#include "cinderx/Interpreter/cinder_opcode_ids.h"

#define _PyOpcode_Caches _CiOpcode_Caches
#define _PyOpcode_Deopt _CiOpcode_Deopt
#define _PyOpcode_OpName _CiOpcode_OpName
#define _PyOpcode_Jump _CiOpcode_Jump
#define _PyOpcode_num_popped _CiOpcode_num_popped
#define _PyOpcode_num_pushed _CiOpcode_num_pushed 
#define _PyOpcode_opcode_metadata _CiOpcode_opcode_metadata


#ifdef Py_INTERNAL_OPCODE_H
#undef Py_INTERNAL_OPCODE_H
#endif
#ifdef Py_OPCODE_H
#undef Py_OPCODE_H
#endif


#ifdef EXTRA_CASES
#undef EXTRA_CASES
#endif
#include "cinderx/Interpreter/cinder_opcode_metadata.h"

#undef _PyOpcode_Caches
#undef _PyOpcode_Deopt
#undef _PyOpcode_OpName
#undef _PyOpcode_Jump
#undef _PyOpcode_num_popped
#undef _PyOpcode_num_pushed 
#undef _PyOpcode_opcode_metadata

#endif
