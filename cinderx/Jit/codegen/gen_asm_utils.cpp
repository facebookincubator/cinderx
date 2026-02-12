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
#elif defined(CINDER_AARCH64)
  env.as->bl(label);
#else
  CINDER_UNSUPPORTED
#endif
  recordDebugEntry(env, instr);
}

void emitCall(Environ& env, uint64_t func, const jit::lir::Instruction* instr) {
#if defined(CINDER_X86_64)
  env.as->call(func);
#elif defined(CINDER_AARCH64)
  // Note that we could do better than this if asmjit knew how to handle arm64
  // relocations for relative calls. That work is done in
  // https://github.com/asmjit/asmjit/issues/499, but as of writing is not yet
  // available.
  env.as->mov(arch::reg_scratch_br, func);
  env.as->blr(arch::reg_scratch_br);
#else
  CINDER_UNSUPPORTED
#endif
  recordDebugEntry(env, instr);
}

} // namespace jit::codegen
