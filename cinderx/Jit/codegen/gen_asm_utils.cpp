// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/gen_asm_utils.h"

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"

namespace jit::codegen {

namespace {
void recordDebugEntry(Environ& env, const jit::lir::Instruction* instr) {
  if (instr->origin() == nullptr) {
    return;
  }
  asmjit::Label addr = env.as->newLabel();
  env.as->bind(addr);
  env.pending_debug_locs.emplace_back(addr, instr->origin());
}
} // namespace

void emitCall(
    Environ& env,
    asmjit::Label label,
    const jit::lir::Instruction* instr) {
#if defined(CINDER_X86_64)
  env.as->call(label);
  recordDebugEntry(env, instr);
#else
  CINDER_UNSUPPORTED
#endif
}

void emitCall(Environ& env, uint64_t func, const jit::lir::Instruction* instr) {
#if defined(CINDER_X86_64)
  env.as->call(func);
  recordDebugEntry(env, instr);
#else
  CINDER_UNSUPPORTED
#endif
}

} // namespace jit::codegen
