// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <string>

namespace jit::lir {

// Map a C helper function to an LIR implementation serialized as a string.
// Return nullptr if there's no such LIR implementation.
const std::string* mapCHelperToLIR(uint64_t addr);

} // namespace jit::lir
