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

#endif // PY_VERSION_HEX >= 0x030C0000
