// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/target_select.h"

#include "cinderx/Jit/codegen/arch/detection.h"

namespace jit::lir {
namespace {

#if defined(CINDER_X86_64)
void selectX64Opcodes(Function* func) {
  (void)func;
}
#elif defined(CINDER_AARCH64)
void selectA64Opcodes(Function* func) {
  (void)func;
}
#else
void selectUnknownTargetOpcodes(Function* func) {
  (void)func;
}
#endif

} // namespace

void selectTargetOpcodes(Function* func) {
#if defined(CINDER_X86_64)
  selectX64Opcodes(func);
#elif defined(CINDER_AARCH64)
  selectA64Opcodes(func);
#else
  selectUnknownTargetOpcodes(func);
#endif
}

} // namespace jit::lir
