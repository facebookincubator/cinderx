// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/arch.h"

namespace cinderx::jit::lir {

class Operand;

// Abstract interface for a register allocator.
//
// A register allocator consumes an LIR function that is using virtual
// registers, assigns each value to a physical register or stack slot, and
// rewrites the LIR in place so that it only refers to physical locations.
class RegisterAllocator {
 public:
  virtual ~RegisterAllocator();

  // Run register allocation, rewriting the function's LIR in place.
  virtual void run() = 0;

  // The set of physical registers modified by the allocated function.
  virtual codegen::PhyRegisterSet getChangedRegs() const = 0;

  // The number of bytes that should be allocated below the base pointer.
  virtual int getFrameSize() const = 0;
};

} // namespace cinderx::jit::lir
