// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace cinderx::jit {

// Build the relocatable ELF object consumed by the GDB JIT interface.
std::vector<std::byte> buildDebugElf(
    std::string_view function_name,
    std::string_view filename,
    int lineno,
    const void* code_addr,
    size_t code_size);

struct CompiledFunctionData;

int register_raw_debug_symbol(
    const char* function_name,
    const char* filename,
    int lineno,
    void* code_addr,
    size_t code_size,
    size_t stack_size);

int register_pycode_debug_symbol(
    PyCodeObject* codeobj,
    const char* fullname,
    jit::CompiledFunctionData& compiled_func);

} // namespace cinderx::jit
