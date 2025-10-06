// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <span>

namespace jit {

// A CodePatcher is used by the runtime to overwrite parts of compiled code.
// Often times this is used to patch in a jump to a deopt exit when an invariant
// that the compiled code relies on is invalidated. It is intended to be used in
// conjunction with the DeoptPatchpoint HIR instruction.
//
// Using a CodePatcher looks roughly like:
//   1. Allocate a CodePatcher.
//
//   2. Allocate a DeoptPatchpoint HIR instruction linked to the CodePatcher
//      from (1) and insert it into the appropriate point in the HIR
//      instruction stream.
//
//   3. Link the CodePatcher from (1) to the appropriate address in the
//      generated code after code generation is complete.
//
// A CodePatcher is only valid for as long as the compiled code to which it is
// linked is alive, so care must be taken not to call `patch()` after the code
// has been destroyed.
class CodePatcher {
 public:
  virtual ~CodePatcher() = default;

  // Link the patcher to a specific location in generated code. This is
  // intended to be called by the JIT after code has been generated but before
  // it is active.
  //
  // `patchpoint` contains the address of the first byte of the patchpoint.
  // `data` contains the bytes that will be written on patching.
  void link(uintptr_t patchpoint, std::span<const uint8_t> data);

  // Overwrite the patchpoint.
  //
  // The patcher must be linked before this can be called.
  void patch();

  // Revert the patchpoint back to a nop.
  //
  // The patcher must be linked before this can be called.
  void unpatch();

  // Check if the patcher has been linked.
  bool isLinked() const;

  // Check if the patcher is currently patched.
  bool isPatched() const;

  // Get where in the code to patch.  Will be nullptr before the patcher is
  // linked.
  uint8_t* patchpoint() const;

  // Get the bytes that are stored within the patcher right now.
  //
  // This either contains the bytes that will be patched in, or the bytes that
  // were there originally.  The former is injected with patch(), the latter can
  // be put back in with unpatch().
  std::span<const uint8_t> storedBytes() const;

 protected:
  // Callback to execute after linking (e.g. subscribing to changes).
  virtual void onLink() {}

  // Callback to execute after patching (e.g. cleaning up the patcher).
  virtual void onPatch() {}

  // Callback to execute after unpatching.
  virtual void onUnpatch() {}

  // Swap data between this object and the actual patchpoint.
  void swap();

  // Where in the code we should patch.
  uint8_t* patchpoint_{nullptr};

  // Data that's written into the patch point.  This is swapped with what's
  // already there, so that this can continuously patch and unpatch.
  //
  // The size of the array here is the total capacity, not necessarily all of it
  // will be patched.
  std::array<uint8_t, 7> data_{};

  // Actual length of the data buffer above, from 0 to 7 bytes.
  uint8_t data_len_ : 7 {0};

  // Whether patch() has been called and a corresponding unpatch() has not yet
  // been called..
  bool is_patched_ : 1 {false};
};

// Subclass of a CodePatcher that is intended for patching in jumps.
class JumpPatcher : public CodePatcher {
 public:
  JumpPatcher();
  ~JumpPatcher() override = default;

  // Specific form of link() for handling jumps.
  //
  // NB: The distance between the patchpoint and the jump target must fit into a
  // signed 32-bit int.
  void linkJump(uintptr_t patchpoint, uintptr_t jump_target);

  // Get the jump target of this patcher.
  uint8_t* jumpTarget() const;
};

} // namespace jit
