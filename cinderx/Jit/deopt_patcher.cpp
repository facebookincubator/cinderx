// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/deopt_patcher.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <cstring>

namespace jit {

// Size of the jump that will be written to the patchpoint
constexpr int kJmpSize = 5;

void DeoptPatcher::emitPatchpoint(asmjit::x86::Builder& as) {
  // 5-byte nop - https://www.felixcloutier.com/x86/nop
  //
  // Asmjit supports multi-byte nops but for whatever reason I can't get it to
  // emit the 5-byte version.
  as.db(0x0f);
  as.db(0x1f);
  as.db(0x44);
  as.db(0x00);
  as.db(0x00);
}

void DeoptPatcher::link(uintptr_t patchpoint, uintptr_t deopt_exit) {
  JIT_CHECK(patchpoint_ == nullptr, "Already linked!");

  auto disp = deopt_exit - (patchpoint + kJmpSize);
  JIT_CHECK(fitsInt32(disp), "Can't encode jump as relative");
  jmp_disp_ = static_cast<int32_t>(disp);
  patchpoint_ = reinterpret_cast<uint8_t*>(patchpoint);

  onLink();
}

void DeoptPatcher::patch() {
  JIT_CHECK(patchpoint_ != nullptr, "Not linked!");

  JIT_DLOG("Patching DeoptPatchPoint at {}", static_cast<void*>(patchpoint_));
  // 32 bit relative jump - https://www.felixcloutier.com/x86/jmp
  patchpoint_[0] = 0xe9;
  std::memcpy(patchpoint_ + 1, &jmp_disp_, sizeof(jmp_disp_));

  onPatch();
}

} // namespace jit
