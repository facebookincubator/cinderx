// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/opcode_stubs.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode_offsets.h"

#include <iterator>
#include <limits>

namespace jit {

// A structured, immutable representation of a CPython bytecode.
//
// This will never directly represent an EXTENDED_ARG bytecode, see opcode() and
// oparg() for more details.
class BytecodeInstruction {
 public:
  BytecodeInstruction(BorrowedRef<PyCodeObject> code, BCOffset baseOffset)
      : code_{code}, baseOffset_{baseOffset} {}

  // Return the position of the first EXTENDED_ARG (if any) making up the full
  // instruction.
  BCOffset baseOffset() const;
  BCIndex baseIndex() const;

  // Return the position of the opcode, skipping over any EXTENDED_ARGs if
  // present.
  BCOffset opcodeOffset() const;
  BCIndex opcodeIndex() const;

  // Get the instruction's opcode or oparg.
  //
  // This will never return an EXTENDED_ARG opcode, those get combined by the
  // BytecodeInstructionBlock into a single BytecodeInstruction.  The multi-byte
  // oparg will be return in one go from oparg().
  int opcode() const;
  int specializedOpcode() const;
  int oparg() const;

  // Check if this instruction is a branch, a return, or a general basic block
  // terminator.
  bool isBranch() const;
  bool isReturn() const;
  bool isTerminator() const;

  // Get the target offset of a successful conditional branch or an
  // unconditional jump.
  BCOffset getJumpTarget() const;

  // Get the offset of the next bytecode in the instruction stream.
  //
  // Will go past the end of the instruction stream for the last instruction.
  BCOffset nextInstrOffset() const;

  BytecodeInstruction nextInstr() const {
    return BytecodeInstruction{code_, nextInstrOffset()};
  }

  bool operator==(const BytecodeInstruction& other) const {
    return code_ == other.code_ && baseOffset_ == other.baseOffset_;
  }

 private:
  void calcOpcodeOffsetAndOparg() const;

  // Return the opcode of the bytecode instruction without instrumentation.
  int uninstrumentedOpcode() const;

  // Get the instruction's code unit (opcode + oparg). The oparg is NOT extended
  // from any prior EXTENDED_ARGs and doesn't include the EXTENDED_OPCODE_FLAG.
  _Py_CODEUNIT word() const;

  // Check if this instruction is a control-flow instruction with an absolute
  // offset as its target.
  bool isAbsoluteControlFlow() const;

  BorrowedRef<PyCodeObject> code_;
  BCOffset baseOffset_;
  mutable BCIndex opcodeIndex_{std::numeric_limits<int>::min()};
  mutable int extendedOparg_{0};
  mutable bool extendedOpcode_{false};
};

// A half open block of bytecode [start, end) viewed as a sequence of
// `BytecodeInstruction`s
//
// Extended args are handled automatically when iterating over the bytecode;
// they will not appear in the stream of `BytecodeInstruction`s.
class BytecodeInstructionBlock {
 public:
  explicit BytecodeInstructionBlock(BorrowedRef<PyCodeObject> code);

  BytecodeInstructionBlock(
      BorrowedRef<PyCodeObject> code,
      BCIndex start,
      BCIndex end);

  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = BytecodeInstruction;
    using pointer = const value_type*;
    using reference = const value_type&;

    Iterator(BorrowedRef<PyCodeObject> code, BCIndex idx, BCIndex end_idx)
        : bci_{code, idx}, end_idx_{end_idx} {}

    bool atEnd() const {
      return bci_.opcodeIndex().value() >= end_idx_;
    }

    reference operator*() {
      JIT_DCHECK(
          !atEnd(), "cannot read past the end of BytecodeInstructionBlock");
      return bci_;
    }

    pointer operator->() {
      JIT_DCHECK(
          !atEnd(), "cannot read past the end of BytecodeInstructionBlock");
      return &bci_;
    }

    Iterator& operator++() {
      bci_ = bci_.nextInstr();
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return bci_ == other.bci_;
    }

    bool operator!=(const Iterator& other) const {
      return !(*this == other);
    }

    // Count the number of remaining bytecode indices in the block.
    //
    // This isn't useful in 3.11+ as instructions are variable length.  So this
    // doesn't tell you anything meaningful. Fortunately, we don't need it
    // beyond 3.10.
    Py_ssize_t remainingIndices() const {
      if constexpr (PY_VERSION_HEX >= 0x030B0000) {
        JIT_ABORT("remainingIndices() not supported in 3.11+");
      }
      return end_idx_ - bci_.opcodeIndex() - 1;
    }

   private:
    BytecodeInstruction bci_;
    BCIndex end_idx_;
  };

  // Get iterators for the beginning and the end of the block.
  Iterator begin() const;
  Iterator end() const;

  // Get the bytecode offset of the first and last instructions in the block.
  BCOffset startOffset() const;
  BCOffset endOffset() const;

  // Get the byte size of the block.
  Py_ssize_t size() const;

  // Get the instruction at the given index.
  BytecodeInstruction at(BCIndex idx) const;

  // Get the block's code object.
  BorrowedRef<PyCodeObject> code() const;

 private:
  ThreadedRef<PyCodeObject> code_;
  BCIndex start_idx_;
  BCIndex end_idx_;
};

#ifndef EXTENDED_OPCODE_FLAG
#define EXTENDED_OPCODE_FLAG 0
#endif

} // namespace jit
