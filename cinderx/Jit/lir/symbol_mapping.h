// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace jit::lir {

// Map the name of a CPython function (e.g. "PyLong_FromLong") to its address.
// Return nullptr if such a function doesn't seem to exist.
const uint64_t* pyFunctionFromName(std::string_view name);

} // namespace jit::lir
