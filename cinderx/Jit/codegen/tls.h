// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace cinderx::jit::codegen {
// Decode the frameless initial-exec form of _PyThreadState_GetCurrent:
//   mrs x?, tpidr_el0
//   add x?, x?, #hi, lsl #12   (optional)
//   add x?, x?, #lo            (optional)
//   ldr x0, [x?]
//   ret
// Returns the TPIDR_EL0-relative offset ((hi << 12) + lo), or -1 if `code`
// doesn't match. Pure word matching, so it is testable on any host.
int32_t decodeFramelessInitialExecOffset(const uint32_t* code);
void initThreadStateOffset();
} // namespace cinderx::jit::codegen
