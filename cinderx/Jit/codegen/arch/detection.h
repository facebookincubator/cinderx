// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

namespace jit::codegen::arch {

// The CPU architecture targeted by the current build.
enum class Arch {
  kX86_64,
  kAarch64,
  kUnknown,
};

// This macro is a marker for places that need platform-specific code.
#define CINDER_UNSUPPORTED

#if defined(__x86_64__)

#define CINDER_X86_64
constexpr Arch kBuildArch = Arch::kX86_64;

#elif defined(__aarch64__)

#define CINDER_AARCH64
constexpr Arch kBuildArch = Arch::kAarch64;

#else

#define CINDER_UNKNOWN
constexpr Arch kBuildArch = Arch::kUnknown;

#endif

} // namespace jit::codegen::arch
