// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/hir/register.h"
#include "cinderx/Jit/stack.h"

namespace jit::hir {

// An entry in the CPython block stack
struct ExecutionBlock {
  // The CPython opcode for the block
  int opcode;

  // Offset in the bytecode of the handler for this block
  BCOffset handler_off;

  // Level to pop the operand stack when the block is exited
  int stack_level;

  bool operator==(const ExecutionBlock& other) const {
    return (opcode == other.opcode) && (handler_off == other.handler_off) &&
        (stack_level == other.stack_level);
  }

  bool operator!=(const ExecutionBlock& other) const {
    return !(*this == other);
  }

  bool isTryBlock() const {
    return opcode == SETUP_FINALLY;
  }

  bool isAsyncForHeaderBlock(const BytecodeInstructionBlock& instrs) const {
    return opcode == SETUP_FINALLY &&
        instrs.at(handler_off).opcode() == END_ASYNC_FOR;
  }
};

using BlockStack = jit::Stack<ExecutionBlock>;
using OperandStack = jit::Stack<Register*>;

// The abstract state of the python frame
struct FrameState {
  FrameState() = default;
  FrameState(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyDictObject> builtins,
      FrameState* parent)
      : code(code), globals(globals), builtins(builtins), parent(parent) {
    JIT_DCHECK(this != parent, "FrameStates should not be self-referential");
  }

  // Used for testing only.
  explicit FrameState(BCOffset bc_off) : cur_instr_offs(bc_off) {}

  FrameState(const FrameState& other) = default;
  FrameState& operator=(const FrameState& other) = default;

  bool operator==(const FrameState& other) const = default;
  bool operator!=(const FrameState& other) const = default;

  // If the function is inlined into another function, the depth at which it
  // is inlined (nested function calls may be inlined). Starts at 1. If the
  // function is not inlined, 0.
  size_t inlineDepth() const {
    int depth = -1;
    for (auto frame = this; frame != nullptr; frame = frame->parent) {
      depth++;
    }
    return depth;
  }

  // The bytecode offset of the current instruction, or -sizeof(_Py_CODEUNIT) if
  // no instruction has executed. This corresponds to the `f_lasti` field of
  // PyFrameObject.
  BCOffset instrOffset() const {
    return cur_instr_offs;
  }

  bool visitUses(const std::function<bool(Register*&)>& func) {
    for (auto& reg : stack) {
      if (!func(reg)) {
        return false;
      }
    }
    for (auto& reg : localsplus) {
      if (reg != nullptr && !func(reg)) {
        return false;
      }
    }
    if (parent != nullptr) {
      return parent->visitUses(func);
    }
    return true;
  }

  // The currently executing instruction.
  BCOffset cur_instr_offs{-static_cast<ssize_t>(sizeof(_Py_CODEUNIT))};

  // Combination of local variables, cell variables (used by closures of inner
  // functions), and free variables (our closure). Locals are at the start and
  // free variables are at the end, but note locals can be cells so there is no
  // guarantee cells are all in the middle.
  std::vector<Register*> localsplus;

  // Number of local variables. Stored as a field directly because in tests
  // there's no code object for us to inspect.
  int nlocals{0};

  OperandStack stack;
  BlockStack block_stack;
  BorrowedRef<PyCodeObject> code;
  BorrowedRef<PyDictObject> globals;
  BorrowedRef<PyDictObject> builtins;

  // Points to the FrameState, if any, into which this was inlined. Used to
  // construct the metadata needed to reify PyFrameObjects for inlined
  // functions during e.g. deopt.
  FrameState* parent{nullptr};
};

} // namespace jit::hir
