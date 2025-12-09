// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bytecode.h"

namespace jit {

BCOffset BytecodeInstruction::baseOffset() const {
  return baseOffset_;
}

BCIndex BytecodeInstruction::baseIndex() const {
  return baseOffset();
}

BCOffset BytecodeInstruction::opcodeOffset() const {
  calcOpcodeOffsetAndOparg();
  return opcodeIndex_;
}

BCIndex BytecodeInstruction::opcodeIndex() const {
  return opcodeOffset();
}

int BytecodeInstruction::opcode() const {
  int op = _Py_OPCODE(word());
  if (extendedOpcode_) {
    return EXTENDED_OPCODE_FLAG | op;
  }
  return op;
}

void BytecodeInstruction::calcOpcodeOffsetAndOparg() const {
  if (opcodeIndex_.value() != std::numeric_limits<int>::min()) {
    return;
  }

  opcodeIndex_ = baseOffset_;
  BCIndex end_idx{countIndices(code_)};
  if (opcodeIndex_.value() >= end_idx) {
    return;
  }

  // Consume all EXTENDED_ARG opcodes until we get to something else.
  auto consume_extended_args = [&] {
    while (_Py_OPCODE(word()) == EXTENDED_ARG) {
      JIT_DCHECK(
          opcodeIndex_.value() < end_idx, "EXTENDED_ARG at end of bytecode");
      extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
      opcodeIndex_++;
    }
  };

  consume_extended_args();

  // Check for EXTENDED_OPCODE, bump forward one opcode if so.
  if (PY_VERSION_HEX >= 0x030E0000 && _Py_OPCODE(word()) == EXTENDED_OPCODE) {
    opcodeIndex_++;
    extendedOparg_ = 0;
    extendedOpcode_ = true;
  }

  // If we had an EXTENDED_OPCODE, then it might also be followed by more
  // EXTENDED_ARGS.
  consume_extended_args();

  extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
}

int BytecodeInstruction::uninstrumentedOpcode() const {
  return uninstrument(code_, opcodeIndex().value());
}

int BytecodeInstruction::specializedOpcode() const {
#if PY_VERSION_HEX >= 0x030C0000
  int opcode = uninstrumentedOpcode();

  switch (opcode) {
    case BINARY_OP_ADD_FLOAT:
    case BINARY_OP_ADD_INT:
    case BINARY_OP_ADD_UNICODE:
    case BINARY_OP_MULTIPLY_FLOAT:
    case BINARY_OP_MULTIPLY_INT:
    case BINARY_OP_SUBTRACT_FLOAT:
    case BINARY_OP_SUBTRACT_INT:
    case BINARY_SUBSCR_DICT:
    case BINARY_SUBSCR_LIST_INT:
    case BINARY_SUBSCR_TUPLE_INT:
    case COMPARE_OP_FLOAT:
    case COMPARE_OP_INT:
    case COMPARE_OP_STR:
    case LOAD_ATTR_MODULE:
    case STORE_SUBSCR_DICT:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
      return opcode;
    default:
      return unspecialize(opcode);
  }
#else
  return opcode();
#endif
}

int BytecodeInstruction::oparg() const {
  calcOpcodeOffsetAndOparg();
  return extendedOparg_;
}

bool BytecodeInstruction::isBranch() const {
  switch (opcode()) {
    case FOR_ITER:
    case JUMP_ABSOLUTE:
    case JUMP_BACKWARD:
    case JUMP_BACKWARD_NO_INTERRUPT:
    case JUMP_FORWARD:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NONZERO_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_ZERO_OR_POP:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_NONE:
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_NOT_NONE:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_ZERO:
    case SEND:
    case SETUP_FINALLY:
      return true;
    default:
      return false;
  }
}

bool BytecodeInstruction::isReturn() const {
  switch (opcode()) {
    case RETURN_CONST:
    case RETURN_PRIMITIVE:
    case RETURN_VALUE:
      return true;
    default:
      return false;
  }
}

bool BytecodeInstruction::isTerminator() const {
  switch (opcode()) {
    case RAISE_VARARGS:
    case RERAISE:
      return true;
    default:
      return isBranch() || isReturn();
  }
}

BCOffset BytecodeInstruction::getJumpTarget() const {
  JIT_DCHECK(
      isBranch(), "Calling getJumpTarget() on a non-branch gives nonsense");

  if (isAbsoluteControlFlow()) {
    return BCIndex{oparg()};
  }

  int delta = oparg();
  if (opcode() == JUMP_BACKWARD || opcode() == JUMP_BACKWARD_NO_INTERRUPT) {
    delta = -delta;
  }
  BCIndex target = BCIndex{nextInstrOffset()} + delta;
  // In 3.11+ the FOR_ITER bytecode encodes a jump to an END_FOR instruction
  // then at runtime it usually dynamically jumps past this. The only time it
  // actually goes through the END_FOR is if the FOR_ITER is operating
  // on a generator and gets adaptively specialized. We always compile
  // unspecialized bytecode so we can always skip the END_FOR.
  //
  // We make this tweak here so it applies both when generating the branching
  // HIR operation, and when creating block boundaries for bytecode. The END_FOR
  // will end up on its own in an unreachable block.
  if (PY_VERSION_HEX >= 0x030B0000 && opcode() == FOR_ITER) {
    BytecodeInstruction target_bc{code_, target};
    JIT_CHECK(target_bc.opcode() == END_FOR, "Expected END_FOR");
    return target_bc.nextInstrOffset();
  }
  return target;
}

BCOffset BytecodeInstruction::nextInstrOffset() const {
  return BCOffset{
      opcodeIndex() + inlineCacheSize(code_, opcodeIndex().value()) + 1};
}

_Py_CODEUNIT BytecodeInstruction::word() const {
#if PY_VERSION_HEX >= 0x030C0000
  int opcode = unspecialize(uninstrumentedOpcode());
  int oparg = _Py_OPARG(codeUnit(code_)[opcodeIndex().value()]);
  return _Py_MAKE_CODEUNIT(opcode, oparg);
#else
  return codeUnit(code_)[opcodeIndex().value()];
#endif
}

bool BytecodeInstruction::isAbsoluteControlFlow() const {
  switch (opcode()) {
    case JUMP_ABSOLUTE:
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_NONZERO_OR_POP:
    case JUMP_IF_NOT_EXC_MATCH:
    case JUMP_IF_TRUE_OR_POP:
    case JUMP_IF_ZERO_OR_POP:
      return true;
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_ZERO:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE:
      // These instructions switched from absolute to relative in 3.11.
      return PY_VERSION_HEX < 0x030B0000;
    default:
      return false;
  }
}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code)
    : BytecodeInstructionBlock{code, BCIndex{0}, BCIndex{countIndices(code)}} {}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code,
    BCIndex start,
    BCIndex end)
    : code_{ThreadedRef<PyCodeObject>::create(code)},
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

BorrowedRef<PyCodeObject> BytecodeInstructionBlock::code() const {
  return code_;
}

} // namespace jit
