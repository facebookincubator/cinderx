// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bytecode.h"

#include "cinderx/UpstreamBorrow/borrowed.h"

#include <atomic>

namespace cinderx::jit {

namespace {

// Load the opcode and oparg from a code object at the given index,
// uninstrumenting the opcode along the way.
//
// Does not do any EXTENDED_ARG or EXTENDED_OPCODE decoding.
_Py_CODEUNIT loadWord(BorrowedRef<PyCodeObject> code, BCIndex idx) {
  static_assert(std::atomic_ref<_Py_CODEUNIT>::is_always_lock_free);

  // Reading from the bytecode stream can race with the specializing interpreter
  // changing the opcode (it will not touch the oparg though).  On free-threaded
  // builds the specializing interpreter uses atomic operations so we use them
  // here as well to be compatible.  However this does nothing to protect
  // against GIL-ful builds of the specializing interpreter...
  //
  // Ideally we would load the bytecode in the preloader where we always hold
  // the GIL, that'll be for a future change.
  auto ref = std::atomic_ref<_Py_CODEUNIT>{codeUnit(code)[idx.value()]};
  auto word = ref.load(std::memory_order_relaxed);

  const int opcode = _Py_OPCODE(word);
  const uint8_t base_opcode = Cix_DEINSTRUMENT(opcode);

  // Opcode was locally instrumented (e.g. INSTRUMENTED_RESUME).
  if (base_opcode != 0) {
    return _Py_MAKE_CODEUNIT(base_opcode, _Py_OPARG(word));
  }

  // Opcode is instrumented via monitoring tables.  Needs the GIL to read the
  // tables as they can also change.
  JIT_THROW_IF(
      opcode == INSTRUMENTED_INSTRUCTION || opcode == INSTRUMENTED_LINE,
      "Instrumented opcode, based on monitoring tables.  We can't read it "
      "without taking the GIL, just punt here.");

  return word;
}

} // namespace

BytecodeInstruction::BytecodeInstruction(
    BorrowedRef<PyCodeObject> code,
    BCOffset baseOffset)
    : code_{code}, baseOffset_{baseOffset} {
  decode();
}

BCOffset BytecodeInstruction::baseOffset() const {
  return baseOffset_;
}

BCIndex BytecodeInstruction::baseIndex() const {
  return baseOffset();
}

BCOffset BytecodeInstruction::opcodeOffset() const {
  return opcodeIndex_;
}

BCIndex BytecodeInstruction::opcodeIndex() const {
  return opcodeOffset();
}

int BytecodeInstruction::opcode() const {
  if (opcodeIndex_.value() < 0) {
    return -1;
  }

  int op = unspecialize(_Py_OPCODE(word()));
  if (extendedOpcode_) {
    return EXTENDED_OPCODE_FLAG | op;
  }
  return op;
}

void BytecodeInstruction::decode() {
  BCIndex end_idx{countIndices(code_)};
  if (baseOffset_.value() < 0 || baseOffset_.value() >= BCOffset{end_idx}) {
    return;
  }

  opcodeIndex_ = baseOffset_;
  word_ = loadWord(code_, opcodeIndex_);

  // Consume all EXTENDED_ARG opcodes until we get to something else.
  auto consume_extended_args = [&] {
    while (_Py_OPCODE(word()) == EXTENDED_ARG) {
      JIT_DCHECK(
          opcodeIndex_.value() < end_idx, "EXTENDED_ARG at end of bytecode");
      extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
      opcodeIndex_++;
      word_ = loadWord(code_, opcodeIndex_);
    }
  };

  consume_extended_args();

  // Check for EXTENDED_OPCODE, bump forward one opcode if so.
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    if (_Py_OPCODE(word()) == EXTENDED_OPCODE) {
      opcodeIndex_++;
      extendedOparg_ = 0;
      extendedOpcode_ = true;
      word_ = loadWord(code_, opcodeIndex_);
    }
  }

  // If we had an EXTENDED_OPCODE, then it might also be followed by more
  // EXTENDED_ARGS.
  consume_extended_args();

  extendedOparg_ = (extendedOparg_ << 8) | _Py_OPARG(word());
}

int BytecodeInstruction::specializedOpcode() const {
  if (opcodeIndex_.value() < 0) {
    return -1;
  }

  int opcode = _Py_OPCODE(word());

  switch (opcode) {
    case BINARY_OP_ADD_FLOAT:
    case BINARY_OP_ADD_INT:
    case BINARY_OP_ADD_UNICODE:
    case BINARY_OP_MULTIPLY_FLOAT:
    case BINARY_OP_MULTIPLY_INT:
    case BINARY_OP_SUBSCR_DICT:
    case BINARY_OP_SUBSCR_LIST_INT:
    case BINARY_OP_SUBSCR_TUPLE_INT:
    case BINARY_OP_SUBTRACT_FLOAT:
    case BINARY_OP_SUBTRACT_INT:
    case BINARY_SUBSCR_DICT:
    case BINARY_SUBSCR_LIST_INT:
    case BINARY_SUBSCR_TUPLE_INT:
    case COMPARE_OP_FLOAT:
    case COMPARE_OP_INT:
    case COMPARE_OP_STR:
    case CONTAINS_OP_DICT:
    case CONTAINS_OP_SET:
    case FOR_ITER_RANGE:
    case LOAD_ATTR_MODULE:
    case STORE_SUBSCR_DICT:
    case STORE_SUBSCR_LIST_INT:
    case TO_BOOL_BOOL:
    case TO_BOOL_INT:
    case TO_BOOL_LIST:
    case TO_BOOL_NONE:
    case TO_BOOL_STR:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
      return opcode;
    default:
      return unspecialize(opcode);
  }
}

int BytecodeInstruction::oparg() const {
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

bool BytecodeInstruction::isBackwardBranch() const {
  return isBranch() && getJumpTarget() <= baseOffset();
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
  if (opcode() == FOR_ITER) {
    BytecodeInstruction target_bc{code_, target};
    JIT_CHECK(target_bc.opcode() == END_FOR, "Expected END_FOR");
    return target_bc.nextInstrOffset();
  }
  return target;
}

BCOffset BytecodeInstruction::nextInstrOffset() const {
  int opcode = _Py_OPCODE(word());
  return BCOffset{opcodeIndex() + _CiOpcode_Caches[unspecialize(opcode)] + 1};
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
      return false;
    default:
      return false;
  }
}

_Py_CODEUNIT BytecodeInstruction::word() const {
  int opcode = _Py_OPCODE(word_);
  int oparg = _Py_OPARG(word_);
  return _Py_MAKE_CODEUNIT(opcode, oparg);
}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code)
    : BytecodeInstructionBlock{code, BCIndex{0}, BCIndex{countIndices(code)}} {}

BytecodeInstructionBlock::BytecodeInstructionBlock(
    BorrowedRef<PyCodeObject> code,
    BCIndex start,
    BCIndex end)
    : code_{code}, start_idx_{start}, end_idx_{end} {}

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

} // namespace cinderx::jit
