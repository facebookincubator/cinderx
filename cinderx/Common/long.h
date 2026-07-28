// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

#include <cstdint>

namespace cinderx {

// Load the PyLongObject for a small integer (from -5 to 256, inclusive).
BorrowedRef<PyLongObject> smallInt(int32_t n);

} // namespace cinderx
