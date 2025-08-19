// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace jit::perf {

constexpr std::string_view kDefaultSymbolPrefix{"__CINDER_INFRA_JIT"};
constexpr std::string_view kFuncSymbolPrefix{"__CINDER_JIT"};
constexpr std::string_view kShadowFrameSymbolPrefix{"__CINDER_SHDW_FRAME_JIT"};

// Write out perf metadata for the given compiled function, depending on what's
// enabled in the environment:
//
// jit_perfmap: If != 0, write out /tmp/perf-<pid>.map for JIT symbols.
//
extern int jit_perfmap;

// perf_jitdump_dir: If non-empty, must be an absolute path to a directory that
//                   exists. A perf jitdump file will be written to this
//                   directory.
extern std::string perf_jitdump_dir;

bool isPreforkCompilationEnabled();

void registerFunction(
    const std::vector<std::pair<void*, std::size_t>>& code_sections,
    std::string_view name,
    std::string_view prefix = kDefaultSymbolPrefix);

// After-fork callback for child processes. Performs any cleanup necessary for
// per-process state, including handling of Linux perf pid maps.
void afterForkChild();

} // namespace jit::perf
