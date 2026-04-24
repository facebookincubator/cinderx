// Copyright (c) Meta Platforms, Inc. and affiliates.

// Use this include instead of Python.h.  This file should be imported before
// any other CPython headers, especially any CPython internal headers used by
// CinderX.  Its purpose is to address incompatibilities between CPython headers
// and our C++ code.

#pragma once

// Avoid conflicts with `min` and `max` on Windows platforms.
#ifdef WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <Python.h>

#if PY_VERSION_HEX >= 0x030E0000
#ifdef __THROW
// mi_decl_throw is defined to be __THROW and breaks in C++ files.
#undef __THROW
#define __THROW
#endif
#include "internal/pycore_mimalloc.h"
#endif

#if defined(__cplusplus)

// clang-format off

// Handle incompatibilities between atomic headers from libgcc and the
// stdatomic.h header from clang.  The headers must be imported in this specific
// order.  If not then builds will fail because of symbol collisions.

// The memory header also has atomic operations in it.
#include <atomic>
#include <memory>

#include <stdatomic.h>

// clang-format on

#endif

// These aren't here because of C vs C++ issues, but rather because they're also
// part of the public Python API and we might as well tack them on.
#include <frameobject.h>
#include <structmember.h>
