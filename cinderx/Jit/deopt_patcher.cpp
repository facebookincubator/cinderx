// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/deopt_patcher.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <cstring>

namespace jit {

void DeoptPatcher::link(uintptr_t patchpoint, uintptr_t deopt_exit) {
  JIT_CHECK(!isLinked(), "Trying to re-link a patcher");

  auto disp = deopt_exit - (patchpoint + kJmpNopBytes.size());
  JIT_CHECK(fitsInt32(disp), "Can't encode jump as relative");

  jmp_disp_ = static_cast<int32_t>(disp);
  patchpoint_ = reinterpret_cast<uint8_t*>(patchpoint);

  onLink();
}

void DeoptPatcher::patch() {
  JIT_CHECK(isLinked(), "Trying to patch a patcher that isn't linked");
  JIT_DLOG("Patching DeoptPatchPoint at {}", static_cast<void*>(patchpoint_));

  // 32 bit relative jump - https://www.felixcloutier.com/x86/jmp
  patchpoint_[0] = 0xe9;
  std::memcpy(patchpoint_ + 1, &jmp_disp_, sizeof(jmp_disp_));

  onPatch();
}

void DeoptPatcher::unpatch() {
  JIT_CHECK(isLinked(), "Trying to unpatch a patcher that isn't linked");
  JIT_DLOG("Unpatching DeoptPatchPoint at {}", static_cast<void*>(patchpoint_));

  std::memcpy(patchpoint_, kJmpNopBytes.data(), kJmpNopBytes.size());

  onUnpatch();
}

bool DeoptPatcher::isLinked() const {
  return patchpoint_ != nullptr;
}

uint8_t* DeoptPatcher::patchpoint() const {
  return patchpoint_;
}

uint8_t* DeoptPatcher::jumpTarget() const {
  return patchpoint_ + jmp_disp_;
}

} // namespace jit
