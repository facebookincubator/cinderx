// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#if defined(__x86_64__)

#define CINDER_X86_64

#else

#define CINDER_UNKNOWN

// This macro is a marker for places that need platform-specific code.
#define CINDER_UNSUPPORTED

#endif
