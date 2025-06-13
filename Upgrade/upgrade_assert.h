// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>
#if PY_VERSION_HEX >= 0x030C0000

#include <stdio.h>
#include <stdlib.h>

#define UPGRADE_ASSERT(tag)                                                    \
  {                                                                            \
    fprintf(stderr, "UPGRADE_ASSERT %s @ %d: %s\n", __FILE__, __LINE__, #tag); \
    abort();                                                                   \
  }

// Essentially structured comments for code which needs upgrading. This should
// be used sparingly as overuse will lead to hard to debug crashes.
#define UPGRADE_NOTE(tag, task)

enum {
  TSTATE_FROM_RUNTIME, // T194018580
  AWAITED_FLAG, // T194027914
  // Missing tstate->frame. T194018580
  FRAME_HANDLING_CHANGED,
  SUPPORT_JIT_INLINING, // T198250666
};

#endif // PY_VERSION_HEX >= 0x030C0000
