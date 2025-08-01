// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace jit {
class CompiledFunction;
}

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
    jit::CompiledFunction* compiled_func);
