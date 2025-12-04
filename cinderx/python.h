// Copyright (c) Meta Platforms, Inc. and affiliates.

// Use this include instead of Python.h.  Its purpose is to avoid
// incompabilities between atomic headers from libgcc and the stdatomic.h header
// from clang.  The headers must be imported in this specific order.  If not
// then builds will fail because of symbol collisions.

#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030E0000
#ifdef __THROW
// mi_decl_throw is defined to be __THROW and breaks in C++ files.
#undef __THROW
#define __THROW
#endif
#include "internal/pycore_mimalloc.h"
#endif

// 3.10 seems to be okay with the header ordering, but it has its own headers
// hitting symbol collisions with atomic.
#if defined(__cplusplus) && PY_VERSION_HEX >= 0x030C0000

// clang-format off

// memory also has atomic operations in it.
#include <atomic>
#include <memory>

#include <stdatomic.h>

// clang-format on

#endif

// These aren't here because of issues with atomics, but rather because they're
// also part of the public Python API and we might as well tack them on.
#include <frameobject.h>
#include <structmember.h>
