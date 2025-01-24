// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/opcode_stubs.h"
#include "cinderx/Interpreter/opcode.h"
#include "cinderx/Jit/bytecode_offsets.h"

#include <iterator>

namespace jit {

// A structured, immutable representation of a CPython bytecode.
//
// This will never directly represent an EXTENDED_ARG bytecode, see opcode() and
// oparg() for more details.
class BytecodeInstruction {
 public:
  BytecodeInstruction(BorrowedRef<PyCodeObject> code, BCOffset offset);

  // Constructor where the oparg is being overwritten because of previous
  // EXTENDED_ARG instructions.
  BytecodeInstruction(
      BorrowedRef<PyCodeObject> code,
      BCOffset offset,
      int oparg);

  // Get the instruction's offset or index in the instruction stream.
  BCOffset offset() const;
  BCIndex index() const;

  // Get the instruction's opcode or oparg.
  //
  // This will never return an EXTENDED_ARG opcode, those get combined by the
  // BytecodeInstructionBlock into a single BytecodeInstruction.  The multi-byte
  // oparg will be return in one go from oparg().
  int opcode() const;
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

 private:
  // Get the instruction's full code unit (opcode + oparg).
  _Py_CODEUNIT word() const;

  // Check if this instruction is a control-flow instruction with an absolute
  // offset as its target.
  bool isAbsoluteControlFlow() const;

  BorrowedRef<PyCodeObject> code_;
  BCOffset offset_;
  int oparg_;
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
        : code_{std::move(code)},
          idx_{idx},
          end_idx_{end_idx},
          bci_{code_, idx, 0} {
      if (!atEnd()) {
        bci_ = BytecodeInstruction{code_, idx};
        consumeExtendedArgs();
      }
    }

    bool atEnd() const {
      return idx_ == end_idx_;
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
      Py_ssize_t increment = 1 + inlineCacheSize(code_, idx_.value());
      // TODO(T137312262): += breaks with how we use CRTP with BCOffsetBase.
      idx_ = idx_ + increment;
      consumeExtendedArgs();
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return code_ == other.code_ && idx_ == other.idx_;
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
      return end_idx_ - idx_ - 1;
    }

   private:
    void consumeExtendedArgs() {
      int accum = 0;
      while (!atEnd() && currentOpcode() == EXTENDED_ARG) {
        accum = (accum << 8) | currentOparg();
        idx_++;
      }
      if (!atEnd()) {
        accum = (accum << 8) | currentOparg();
        bci_ = BytecodeInstruction{code_, idx_, accum};
      }
    }

    _Py_CODEUNIT* currentInstr() const {
      return codeUnit(code_) + idx_.value();
    }

    int currentOpcode() const {
      JIT_DCHECK(
          !atEnd(),
          "Trying to access bytecode instruction past end of code object");
      return _Py_OPCODE(*currentInstr());
    }

    int currentOparg() const {
      JIT_DCHECK(
          !atEnd(),
          "Trying to access bytecode instruction past end of code object");
      return _Py_OPARG(*currentInstr());
    }

    // Not stored as a Ref because that would make Iterator non-copyable.
    BorrowedRef<PyCodeObject> code_;
    BCIndex idx_;
    BCIndex end_idx_;
    BytecodeInstruction bci_;
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
  Ref<PyCodeObject> code_;
  BCIndex start_idx_;
  BCIndex end_idx_;
};

} // namespace jit
