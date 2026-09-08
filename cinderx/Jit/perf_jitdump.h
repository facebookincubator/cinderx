// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace cinderx::jit::perf {

// Prefix to use for JIT-compiled Python functions.
constexpr std::string_view kFuncSymbolPrefix{"__CINDER_JIT"};

// Prefix to use for JIT-compiled code internal to CinderX, such as shared
// trampolines.
constexpr std::string_view kInternalSymbolPrefix{"__CINDER_INFRA_JIT"};

bool isPreforkCompilationEnabled();

void registerFunction(
    const std::vector<std::pair<void*, std::size_t>>& code_sections,
    std::string_view name,
    std::string_view prefix);

// After-fork callback for child processes. Performs any cleanup necessary for
// per-process state, including handling of Linux perf pid maps.
void afterForkChild();

} // namespace cinderx::jit::perf
