// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

// This file defines macros that allow use of compiler-specific features in a
// portable way.

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(noinline)
#define CINDERX_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define CINDERX_NOINLINE __declspec(noinline)
#else
#define CINDERX_NOINLINE
#endif

#if __has_attribute(always_inline)
#define CINDERX_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define CINDERX_ALWAYS_INLINE __forceinline
#else
#define CINDERX_ALWAYS_INLINE inline
#endif
