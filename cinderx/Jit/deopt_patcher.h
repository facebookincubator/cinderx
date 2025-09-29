// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <array>
#include <cstdint>

namespace jit {

// 5-byte nop - https://www.felixcloutier.com/x86/nop
//
// Asmjit supports multi-byte nops but for whatever reason we can't get it to
// emit the 5-byte version.
constexpr auto kJmpNopBytes =
    std::to_array<uint8_t>({0x0f, 0x1f, 0x44, 0x00, 0x00});

// A DeoptPatcher is used by the runtime to invalidate compiled code when an
// invariant that the compiled code relies on is invalidated. It is intended
// to be used in conjunction with the DeoptPatchpoint HIR instruction.
//
// Using a DeoptPatcher looks roughly like:
//   1. Allocate a DeoptPatcher.
//   2. Allocate a DeoptPatchpoint HIR instruction linked to the DeoptPatcher
//      from (1) and insert it into the appropriate point in the HIR
//      instruction stream. The DeoptPatcher from (1) will be linked to the
//      appropriate point in the generated code by the JIT.
//
// A DeoptPatcher is only valid for as long as the compiled code to which it is
// linked is alive, so care must be taken not to call `patch()` after the code
// has been destroyed.
//
// We implement this by writing a 5-byte nop into the generated code at the
// point that we want to patch/invalidate. As a future optimization, we may be
// able to avoid reserving some/all space for the patchpoint (e.g. if we can
// prove that none of the 5-bytes following it are the target of a jump and they
// will never be unpatched).
class DeoptPatcher {
 public:
  virtual ~DeoptPatcher() = default;

  // Link the patcher to a specific location in generated code. This is
  // intended to be called by the JIT after code has been generated but before
  // it is active.
  //
  // `patchpoint` contains the address of the first byte of the patchpoint.
  // `deopt_exit` contains the address of the deopt exit that we'll jump to when
  // patched.
  //
  // NB: The distance between the patchpoint and the deopt exit must fit into
  // a signed 32 bit int.
  void link(uintptr_t patchpoint, uintptr_t deopt_exit);

  // Overwrite the patchpoint with a deopt.
  //
  // The patcher must be linked before this can be called.
  void patch();

  // Revert the patchpoint back to a nop.
  //
  // The patcher must be linked before this can be called.
  void unpatch();

  // Check if the patcher has been linked.
  bool isLinked() const;

  // Get where in the code to patch.  Will be nullptr before the patcher is
  // linked.
  uint8_t* patchpoint() const;

  // Get the address where the patched code will jump.  Will be nullptr before
  // the patcher is linked.
  uint8_t* jumpTarget() const;

 protected:
  // Callback to execute after linking (e.g. subscribing to changes).
  virtual void onLink() {}

  // Callback to execute after patching (e.g. cleaning up the patcher).
  virtual void onPatch() {}

  // Callback to execute after unpatching.
  virtual void onUnpatch() {}

 private:
  // Where in the code we should patch
  uint8_t* patchpoint_{nullptr};

  // Displacement used by the jump that is written into the patchpoint.  It
  // should jump to the appropriate deopt exit.
  int32_t jmp_disp_{0};
};

} // namespace jit
