// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"
#include "internal/pycore_opcode_metadata.h"

#ifndef Ci_INTERNAL_OPCODE_H
#define Ci_INTERNAL_OPCODE_H

#ifndef EXTENDED_OPCODE
#define EXTENDED_OPCODE 126
#endif

#include "cinderx/Interpreter/cinder_opcode_ids.h"

#define _CiOpcode_Deopt _PyOpcode_Deopt
#define _CiOpcode_Caches _PyOpcode_Caches
#endif
