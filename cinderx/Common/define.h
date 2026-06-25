// Copyright (c) Meta Platforms, Inc. and affiliates.

// A collection of preprocessor defines converted into `constexpr` values that
// can be used unconditionally.
//
// This header should never depend on any other file.

#pragma once

constexpr bool kPyDebug =
#ifdef Py_DEBUG
    true;
#else
    false;
#endif

constexpr bool kPyRefDebug =
#ifdef Py_REF_DEBUG
    true;
#else
    false;
#endif

// True when CinderX is built against a free-threaded (Py_GIL_DISABLED) Python.
//
// When false, code can assume the GIL is held.  When true, it cannot, the GIL
// might still be held at any given moment but that's no longer guaranteed.
constexpr bool kFreeThreadedBuild =
#ifdef Py_GIL_DISABLED
    true;
#else
    false;
#endif

// True when CinderX is built for the prefork (fork-and-exec) process model,
// i.e. with the ENABLE_PREFORK_MODEL build flag.  In this mode some behaviors
// that would otherwise be runtime options are forced on at compile time -- e.g.
// JIT-compiled functions are always immortalized, avoiding refcount churn that
// would otherwise be copied-on-write across forked worker processes.
//
// Prefer branching on this constexpr over #ifdef ENABLE_PREFORK_MODEL so the
// guarded code still gets type-checked in every build configuration.
constexpr bool kPreforkModel =
#ifdef ENABLE_PREFORK_MODEL
    true;
#else
    false;
#endif
