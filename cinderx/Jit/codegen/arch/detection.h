// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#if defined(__x86_64__)

#define CINDER_X86_64

#elif defined(__aarch64__)

#define CINDER_AARCH64

// This is here until we have aarch64 support everywhere.
#define CINDER_UNSUPPORTED

#else

#define CINDER_UNKNOWN

// This macro is a marker for places that need platform-specific code.
#define CINDER_UNSUPPORTED

#endif
