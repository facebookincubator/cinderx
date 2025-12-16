// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <stdint.h>

// Extra data attached to a code object.
typedef struct CodeExtra {
  union {
    // Number of times the code object has been called.
    uint64_t calls;
    // Used for unallocated free list code extras
    struct CodeExtra* next;
  };
} CodeExtra;
