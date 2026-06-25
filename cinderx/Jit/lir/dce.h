// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/function.h"

namespace cinderx::jit::lir {

void eliminateDeadCode(Function* func);

} // namespace cinderx::jit::lir
