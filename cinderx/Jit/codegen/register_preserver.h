// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/arch.h"

#include <vector>

namespace jit::codegen {

// RegisterPreserver helps with preserving and restoring registers during code
// generation. It handles both general-purpose and vector registers, and ensures
// proper stack alignment.
class RegisterPreserver {
 public:
  RegisterPreserver(
      arch::Builder* as,
      const std::vector<std::pair<const arch::Reg&, const arch::Reg&>>&
          save_regs);

  // Save all registers in the save_regs_ list to the stack
  void preserve();

  // Restore all registers from the stack in reverse order
  void restore();

  // Remaps the registers as if they had been preserved and restored
  void remap();

 private:
#if defined(CINDER_AARCH64)
  enum class RegisterGroup : uint8_t { kGp, kVecD, kGpPair, kVecDPair };

  // Computes register groupings for stp/ldp pairing. Returns a vector of
  // (index, length) where length is 1 or 2.
  std::vector<std::pair<size_t, RegisterGroup>> registerGroups() const;
#endif

  [[maybe_unused]] arch::Builder* as_;
  [[maybe_unused]] const std::vector<
      std::pair<const arch::Reg&, const arch::Reg&>>& save_regs_;
  [[maybe_unused]] bool align_stack_;
};

} // namespace jit::codegen
