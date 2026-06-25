// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

namespace cinderx::jit::lir {

class Function;

// Select target-specific LIR opcodes from generic LIR opcodes.
//
// This is currently a phase boundary for future target-specific instruction
// selection. It intentionally operates on the existing LIR function rather than
// introducing a separate IR.
void selectTargetOpcodes(Function* func);

} // namespace cinderx::jit::lir
