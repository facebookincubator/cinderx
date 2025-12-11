// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Interpreter/cinder_opcode_ids.h"

#ifdef NEED_OPCODE_NAMES

// Note: Some opcodes share the same number (e.g., HAVE_ARGUMENT and STORE_NAME
// are both 90). The designated initializer will use the last one.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#endif

static const char *const _CiOpcode_OpName[256] = {
#define OPCODE_NAME(name, num) [num] = #name,
    PY_OPCODES(OPCODE_NAME)
#undef OPCODE_NAME
};

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif
