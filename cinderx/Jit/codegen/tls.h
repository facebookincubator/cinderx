// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace cinderx::jit::codegen {

// Decode the x86-64 initial-exec / local-exec forms of
// _PyThreadState_GetCurrent:
//   push %rbp; mov %rsp,%rbp
//   mov %fs:OFFSET,%rax
//   pop %rbp; ret
// or, with the thread pointer loaded first:
//   push %rbp; mov %rsp,%rbp
//   mov %fs:0x0,%rax
//   mov OFFSET(%rax),%rax
//   pop %rbp; ret
// Returns the %fs-relative OFFSET, or -1 if `code` doesn't match. Pure byte
// matching, so it is testable on any host.
int32_t decodeInitialExecOffset(const uint8_t* code);

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

// Whether compiled code should read the thread state through the TLS offset
// found by initThreadStateOffset(), rather than calling
// _PyThreadState_GetCurrent().
bool canLoadThreadStateFromTls();

} // namespace cinderx::jit::codegen
