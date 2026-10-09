// Copyright (c) Meta Platforms, Inc. and affiliates.

// Use this include instead of Python.h.  This file should be imported before
// any other CPython headers, especially any CPython internal headers used by
// CinderX.  Its purpose is to address incompatibilities between CPython headers
// and our C++ code.

#pragma once

// Everything below tests _WIN32 rather than WIN32, which the rest of CinderX
// uses: this header is included by targets that don't go through CinderX's Buck
// macros (`_cinderx.cpp` is a plain cpp_python_extension), so -DWIN32 does not
// reach it.  _WIN32 is predefined by every Windows-targeting compiler.
#ifdef _WIN32
// Pull in the compiler's intrinsics before windows.h.  winnt.h declares a few
// of them itself (e.g. _m_prefetchw), and if it gets there first clang's own
// *intrin.h headers redeclare them with a different language linkage.
#include <intrin.h>

// Avoid conflicts with `min` and `max` on Windows platforms.
#define NOMINMAX
#include <windows.h>
// Avoid conflicts with the `small` macro defined by Windows headers.
#undef small
#endif

#include <Python.h>

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
// Override CPython's check so CinderX doesn't link against its JIT executable
// type or destructor, letting the same binary load into runtimes that define
// the type statically or per-interpreter.  This must precede the internal frame
// headers, whose inline functions expand the check.
#ifdef PyUnstable_JITExecutable_Check
#error "cinderx/python.h must be included before CPython's internal headers"
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Destructor of the runtime's JIT executable type, taken from an instance
// created during CinderX initialization. Stored in ModuleState.
destructor Ci_GetJITExecutableDealloc(void);
#ifdef __cplusplus
}
#endif

#define PyUnstable_JITExecutable_Check(op) \
  (Py_TYPE(op)->tp_dealloc == Ci_GetJITExecutableDealloc())
#endif

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

// The MSVC STL declares the C11 fence functions in <atomic> itself, so pulling
// in <stdatomic.h> on top of it gives conflicting declarations.
#ifndef _WIN32
#include <stdatomic.h>
#endif

// clang-format on

#endif

// These aren't here because of C vs C++ issues, but rather because they're also
// part of the public Python API and we might as well tack them on.
#include <frameobject.h>
#include <structmember.h>
