// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bytecode.h"

#include <unordered_set>

namespace jit {

// these must be opcodes whose oparg is a jump target index
const std::unordered_set<int> kBranchOpcodes = {
    FOR_ITER,
    JUMP_ABSOLUTE,
    JUMP_BACKWARD,
    JUMP_BACKWARD_NO_INTERRUPT,
    JUMP_FORWARD,
    JUMP_IF_FALSE_OR_POP,
    JUMP_IF_NONZERO_OR_POP,
    JUMP_IF_NOT_EXC_MATCH,
    JUMP_IF_TRUE_OR_POP,
    JUMP_IF_ZERO_OR_POP,
    POP_JUMP_IF_FALSE,
    POP_JUMP_IF_NONE,
    POP_JUMP_IF_NONZERO,
    POP_JUMP_IF_NOT_NONE,
    POP_JUMP_IF_TRUE,
    POP_JUMP_IF_ZERO,
};

const std::unordered_set<int> kRelBranchOpcodes = {
    FOR_ITER,
    JUMP_BACKWARD,
    JUMP_BACKWARD_NO_INTERRUPT,
    JUMP_FORWARD,
    POP_JUMP_IF_NONE,
    POP_JUMP_IF_NOT_NONE,
    SEND,
    SETUP_FINALLY,

#if PY_VERSION_HEX >= 0x030B0000
    // These instructions switched from absolute to relative in 3.11.
    POP_JUMP_IF_FALSE,
    POP_JUMP_IF_TRUE,
#endif
};

// we always consider branches block terminators; no need to duplicate them here
const std::unordered_set<int> kBlockTerminatorOpcodes = {
    RAISE_VARARGS,
    RERAISE,
    RETURN_CONST,
    RETURN_PRIMITIVE,
    RETURN_VALUE,
};

BytecodeInstruction::BytecodeInstruction(
    BorrowedRef<PyCodeObject> code,
    BCOffset offset)
    : code_{code}, offset_{offset}, oparg_{_Py_OPARG(word())} {}

BytecodeInstruction::BytecodeInstruction(
    BorrowedRef<PyCodeObject> code,
    BCOffset offset,
    int oparg)
    : code_{code}, offset_{offset}, oparg_{oparg} {}

BCOffset BytecodeInstruction::offset() const {
  return offset_;
}

BCIndex BytecodeInstruction::index() const {
  return offset();
}

int BytecodeInstruction::opcode() const {
  return _Py_OPCODE(word());
}

int BytecodeInstruction::oparg() const {
  return oparg_;
}

bool BytecodeInstruction::isBranch() const {
  return kBranchOpcodes.count(opcode());
}

bool BytecodeInstruction::isReturn() const {
  return opcode() == RETURN_VALUE || opcode() == RETURN_PRIMITIVE;
}

bool BytecodeInstruction::isTerminator() const {
  return isBranch() || kBlockTerminatorOpcodes.count(opcode());
}

BCOffset BytecodeInstruction::getJumpTarget() const {
  JIT_DCHECK(
      isBranch(), "Calling getJumpTarget() on a non-branch gives nonsense");

  if (!kRelBranchOpcodes.count(opcode())) {
    return BCIndex{oparg()};
  }

  int delta = oparg();
  if (opcode() == JUMP_BACKWARD || opcode() == JUMP_BACKWARD_NO_INTERRUPT) {
    delta = -delta;
  }
  // If the iterator ended normally, we need to jump forward oparg,
  // then skip following END_FOR instruction.
  if (PY_VERSION_HEX >= 0x030B0000 && opcode() == FOR_ITER) {
    delta += 1;
  }
  return BCIndex{nextInstrOffset()} + delta;
}

BCOffset BytecodeInstruction::nextInstrOffset() const {
  return BCOffset{index() + inlineCacheSize(code_, index().value()) + 1};
}

_Py_CODEUNIT BytecodeInstruction::word() const {
  return codeUnit(code_)[index().value()];
}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code)
    : BytecodeInstructionBlock{code, BCIndex{0}, BCIndex{countIndices(code)}} {}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code,
    BCIndex start,
    BCIndex end)
    : code_{Ref<PyCodeObject>::create(code)},
      start_idx_{start},
      end_idx_{end} {}

BytecodeInstructionBlock::Iterator BytecodeInstructionBlock::begin() const {
  return Iterator{code_, start_idx_, end_idx_};
}

BytecodeInstructionBlock::Iterator BytecodeInstructionBlock::end() const {
  return Iterator{code_, end_idx_, end_idx_};
}

BCOffset BytecodeInstructionBlock::startOffset() const {
  return start_idx_;
}

BCOffset BytecodeInstructionBlock::endOffset() const {
  return end_idx_;
}

Py_ssize_t BytecodeInstructionBlock::size() const {
  return end_idx_ - start_idx_;
}

BytecodeInstruction BytecodeInstructionBlock::at(BCIndex idx) const {
  JIT_CHECK(
      idx >= start_idx_ && idx < end_idx_,
      "Invalid index {}, bytecode block is [{}, {})",
      idx,
      start_idx_,
      end_idx_);
  return BytecodeInstruction{code_, idx};
}

BytecodeInstruction BytecodeInstructionBlock::lastInstr() const {
  JIT_CHECK(size() > 0, "Block has no instructions");
  return BytecodeInstruction(code_, end_idx_ - 1);
}

BorrowedRef<PyCodeObject> BytecodeInstructionBlock::code() const {
  return code_;
}

} // namespace jit
