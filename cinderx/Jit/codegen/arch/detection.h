// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/define.h"

namespace cinderx::jit::codegen::arch {

// For temporary backwards compatibility, prefer using the `cinderx` namespaced
// symbols rather than `cinderx::jit::codegen::arch`.

using cinderx::Arch;

constexpr Arch kBuildArch = cinderx::kBuildArch;

} // namespace cinderx::jit::codegen::arch
