// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>

namespace cinderx {

// Thread names on Linux are 16 bytes, including the NUL terminator.
constexpr size_t kMaxThreadNameLen{15};

// Set the current thread's name.
void setThreadName(const char* name);

} // namespace cinderx
