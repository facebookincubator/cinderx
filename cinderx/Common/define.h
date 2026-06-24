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
