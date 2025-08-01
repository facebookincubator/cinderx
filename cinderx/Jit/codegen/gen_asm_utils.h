// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/lir/instruction.h"

#include <asmjit/asmjit.h>

namespace jit::codegen {

struct Environ;

// Emit a call and record the unit state at the program point following the
// call.
//
// Use this when emitting calls from custom actions. This will update the JIT's
// internal metadata so that the location in the generated code can be mapped
// back to the bytecode instruction that produced it.
void emitCall(
    Environ& env,
    asmjit::Label label,
    const jit::lir::Instruction* instr);
void emitCall(Environ& env, uint64_t func, const jit::lir::Instruction* instr);

} // namespace jit::codegen
