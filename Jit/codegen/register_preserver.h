// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <asmjit/asmjit.h>

#include <vector>

namespace jit::codegen {

// RegisterPreserver helps with preserving and restoring registers during code
// generation. It handles both general-purpose and XMM registers, and ensures
// proper stack alignment.
class RegisterPreserver {
 public:
  RegisterPreserver(
      asmjit::x86::Builder* as,
      const std::vector<
          std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>&
          save_regs);

  // Save all registers in the save_regs_ list to the stack
  void preserve();

  // Restore all registers from the stack in reverse order
  void restore();

 private:
  asmjit::x86::Builder* as_;
  const std::vector<
      std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>& save_regs_;
  bool align_stack_;
};

} // namespace jit::codegen
