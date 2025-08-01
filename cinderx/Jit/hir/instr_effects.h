// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bitvector.h"
#include "cinderx/Jit/hir/alias_class.h"

namespace jit::hir {

class Instr;

// The memory effects of an instruction, both on the reference counts of its
// inputs/output, and its side-effects on other memory locations.
struct MemoryEffects {
  // If true, the instruction produces a borrowed reference to a PyObject*. If
  // false, the instruction either produces an owned output, no output, or a
  // value that isn't a PyObject*.
  bool borrows_output{false};

  // When borrows_output is true, this will indicate the memory location that
  // supports that borrowed reference. It will be AEmpty for values that are
  // safe to borrow for the lifetime of the containing function (like members
  // of co_consts).
  AliasClass borrow_support;

  // A bitvector with a bit for every operand of the instruction, each of which
  // is set to 1 if the instruction steals a reference to that operand.
  util::BitVector stolen_inputs;

  // Memory locations that this instruction may write to.
  AliasClass may_store;
};

MemoryEffects memoryEffects(const Instr& inst);

// Return true if the instruction may call arbitrary (Python) code.
bool hasArbitraryExecution(const Instr& inst);

} // namespace jit::hir
