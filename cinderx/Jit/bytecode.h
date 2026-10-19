// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/opcode_stubs.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode_offsets.h"

#include <iterator>

namespace cinderx::jit {

// A structured, immutable representation of a CPython bytecode.  The opcode and
// argument are captured on construction and retained even if the interpreter
// subsequently specializes or instruments the live bytecode.
//
// This will never directly represent an EXTENDED_ARG bytecode, see opcode() and
// oparg() for more details.
class BytecodeInstruction {
 public:
  BytecodeInstruction(BorrowedRef<PyCodeObject> code, BCOffset baseOffset);

  // Return the position of the first EXTENDED_ARG (if any) making up the full
  // instruction.
  BCOffset baseOffset() const;
  BCIndex baseIndex() const;

  // Return the position of the opcode, skipping over any EXTENDED_ARGs if
  // present.
  BCOffset opcodeOffset() const;
  BCIndex opcodeIndex() const;

  // Get the instruction's opcode or oparg.  Opcodes are unspecialized and
  // uninstrumented.
  //
  // This will never return an EXTENDED_ARG opcode, those get combined by the
  // BytecodeInstructionBlock into a single BytecodeInstruction.  The multi-byte
  // oparg will be returned in one go from oparg().
  int opcode() const;
  int oparg() const;

  // Like opcode(), but return the specialized form of the opcode
  // (e.g. FOR_ITER_RANGE is specialized from FOR_ITER).
  //
  // Note: If the GIL isn't held, the specializing interpreter can change the
  // opcode at any time.  This class snapshots it so the same
  // BytecodeInstruction instance will always give you the same consistent
  // result, but the code object can be changing underneath.
  int specializedOpcode() const;

  // Check if this instruction is a branch, a return, or a general basic block
  // terminator.
  bool isBranch() const;
  bool isBackwardBranch() const;
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
  // Decode all EXTENDED_ARG and EXTENDED_OPCODE instructions and compute this
  // instruction's true opcode, oparg, and index.
  void decode();

  // Check if this instruction is a control-flow instruction with an absolute
  // offset as its target.
  bool isAbsoluteControlFlow() const;

  // Get the instruction's code unit (opcode + oparg). The oparg is NOT extended
  // from any prior EXTENDED_ARGs and doesn't include the EXTENDED_OPCODE_FLAG.
  // The opcode will be uninstrumented but it can be specialized.
  //
  // This is stable after the object is constructed.  The specializing
  // interpreter can specialize or unspecialize the opcode in the code object
  // and this will not report the change.
  _Py_CODEUNIT word() const;

  // Code object the bytecode instruction found in.
  BorrowedRef<PyCodeObject> code_;
  // Offset where the bytecode instructions begins.  This might point to a
  // number of EXTENDED_ARG or EXTENDED_OPCODE that will need to be decoded
  // first.
  BCOffset baseOffset_;

  // Index of the actual opcode, after EXTENDED_ARG and EXTENDED_OPCODE
  // instructions are decoded.
  BCIndex opcodeIndex_{-1};
  // Value of the oparg, after all EXTENDED_ARG instructions are decoded.
  int extendedOparg_{0};
  // The (opcode,oparg) pair at opcodeIndex_.  The opcode is uninstrumented but
  // can be specialized.
  _Py_CODEUNIT word_{};
  // Whether the opcode was defined with EXTENDED_OPCODE.
  bool extendedOpcode_{false};
};

// A half open block of bytecode [start, end) viewed as a sequence of
// `BytecodeInstruction`s
//
// Extended args are handled automatically when iterating over the bytecode;
// they will not appear in the stream of `BytecodeInstruction`s.
//
// BytecodeInstructionBlock borrows the code object - it does NOT keep it alive.
// Callers must ensure the code object outlives the block, which is true for
// all current uses (HIR building holds a Ref<> to the code
// object in Preloader / Function).
class BytecodeInstructionBlock {
 public:
  explicit BytecodeInstructionBlock(BorrowedRef<PyCodeObject> code);

  BytecodeInstructionBlock(
      BorrowedRef<PyCodeObject> code,
      BCIndex start,
      BCIndex end);

  ~BytecodeInstructionBlock() = default;

  BytecodeInstructionBlock(BytecodeInstructionBlock&&) = default;
  BytecodeInstructionBlock& operator=(BytecodeInstructionBlock&&) = default;

  BytecodeInstructionBlock(const BytecodeInstructionBlock&) = delete;
  BytecodeInstructionBlock& operator=(const BytecodeInstructionBlock&) = delete;

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
      return bci_.baseIndex() >= end_idx_;
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
  BorrowedRef<PyCodeObject> code_;
  BCIndex start_idx_;
  BCIndex end_idx_;
};

#ifndef EXTENDED_OPCODE_FLAG
#define EXTENDED_OPCODE_FLAG 0
#endif

} // namespace cinderx::jit
