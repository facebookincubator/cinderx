// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/gen_asm_utils.h"

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/generators_rt.h"

using namespace asmjit;

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
  env.as->bl(func);
#else
  CINDER_UNSUPPORTED
#endif
  recordDebugEntry(env, instr);
}

void RestoreOriginalGeneratorFramePointer(arch::Builder* as) {
#if defined(CINDER_X86_64)
  size_t original_frame_pointer_offset =
      offsetof(GenDataFooter, originalFramePointer);
  as->mov(x86::rbp, x86::ptr(x86::rbp, original_frame_pointer_offset));
#elif defined(CINDER_AARCH64)
  size_t original_frame_pointer_offset =
      offsetof(GenDataFooter, originalFramePointer);
  as->ldr(
      arch::fp,
      arch::ptr_resolve(
          as, arch::fp, original_frame_pointer_offset, arch::reg_scratch_0));
#else
  CINDER_UNSUPPORTED
#endif
}

} // namespace jit::codegen
