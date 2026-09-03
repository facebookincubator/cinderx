// Copyright (c) Meta Platforms, Inc. and affiliates.

// A collection of preprocessor defines converted into `constexpr` values that
// can be used unconditionally.
//
// This header should never depend on any other file, except for
// cinderx/python.h.

#pragma once

#include "cinderx/python.h"

#if defined(__has_feature)
#define CINDER_HAS_FEATURE(x) __has_feature(x)
#else
#define CINDER_HAS_FEATURE(x) 0
#endif

#if defined(__SANITIZE_THREAD__) || CINDER_HAS_FEATURE(thread_sanitizer)
#define CINDER_TSAN_ENABLED 1
#else
#define CINDER_TSAN_ENABLED 0
#endif

namespace cinderx {

// Whether CinderX is being built with a debug build configuration.
constexpr bool kDebug =
#ifdef NDEBUG
    false;
#else
    true;
#endif

// Whether the Python runtime was built with a debug build configuration.
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

// True when CinderX is built with ThreadSanitizer, on any architecture or OS.
//
// Prefer branching on this constexpr over #if CINDER_TSAN_ENABLED so the
// guarded code still gets type-checked in every build configuration.
constexpr bool kTsanEnabled = CINDER_TSAN_ENABLED;

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

// The CPU architecture targeted by the current build.
enum class Arch {
  kX86_64,
  kAarch64,
  kUnknown,
};

// This macro is a marker for places that need platform-specific code.
#define CINDER_UNSUPPORTED

#if defined(__x86_64__) || defined(_M_AMD64)

#define CINDER_X86_64
constexpr Arch kBuildArch = Arch::kX86_64;

#elif defined(__aarch64__)

#define CINDER_AARCH64
constexpr Arch kBuildArch = Arch::kAarch64;

#else

#define CINDER_UNKNOWN
constexpr Arch kBuildArch = Arch::kUnknown;

#endif

// Operating system being targeted by the current build.
enum class OS {
  kUnknown,
  kLinux,
  kMacOS,
  kWindows,
};

#if defined(__linux__)
constexpr OS kOS = OS::kLinux;
#elif defined(__APPLE__)
constexpr OS kOS = OS::kMacOS;
#elif defined(_WIN32)
constexpr OS kOS = OS::kWindows;
#else
constexpr OS kOS = OS::kUnknown;
#endif

} // namespace cinderx
