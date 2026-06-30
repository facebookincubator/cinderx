// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

namespace cinderx::jit::lir {

class Function;

// Select and legalize target-specific LIR from generic LIR.
//
// This pass handles target-specific opcode selection and local legalization of
// instruction forms or data types. Legalization here must be self-contained: it
// runs after post-generation fixed-point rewrites and should not create LIR
// that depends on those rewrites running again.
void selectTargetOpcodes(Function* func);

} // namespace cinderx::jit::lir
