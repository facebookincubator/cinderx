// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/opcode_stubs.h"
#include "cinderx/Interpreter/opcode.h"

#include "cinderx/Jit/bytecode_offsets.h"

#include <Python.h>

#include <iterator>
#include <unordered_set>

namespace jit {

extern const std::unordered_set<int> kBranchOpcodes;
extern const std::unordered_set<int> kRelBranchOpcodes;
extern const std::unordered_set<int> kBlockTerminatorOpcodes;

// A structured, immutable representation of a CPython bytecode.
//
// This will never represent an EXTENDED_ARG bytecode.  That gets folded in via
// BytecodeInstructionBlock, and the resulting BytecodeInstruction has the
// relevant opcode plus a multi-byte oparg.
class BytecodeInstruction {
 public:
  BytecodeInstruction(BorrowedRef<PyCodeObject> code, BCOffset offset)
      : code_{code}, offset_{offset}, oparg_{_Py_OPARG(word())} {}

  // Constructor where the oparg is being overwritten because of previous
  // EXTENDED_ARG instructions.
  BytecodeInstruction(
      BorrowedRef<PyCodeObject> code,
      BCOffset offset,
      int oparg)
      : code_{code}, offset_{offset}, oparg_{oparg} {}

  BCOffset offset() const {
    return offset_;
  }

  BCIndex index() const {
    return offset();
  }

  int opcode() const {
    return _Py_OPCODE(word());
  }

  int oparg() const {
    return oparg_;
  }

  bool IsBranch() const {
    return kBranchOpcodes.count(opcode());
  }

  bool IsReturn() const {
    return opcode() == RETURN_VALUE || opcode() == RETURN_PRIMITIVE;
  }

  bool IsTerminator() const {
    return IsBranch() || kBlockTerminatorOpcodes.count(opcode());
  }

  BCOffset getJumpTarget() const {
    JIT_DCHECK(
        IsBranch(), "Calling getJumpTarget() on a non-branch gives nonsense");

    if (!kRelBranchOpcodes.count(opcode())) {
      return BCIndex{oparg()};
    }

    int delta = oparg();
    if (opcode() == JUMP_BACKWARD || opcode() == JUMP_BACKWARD_NO_INTERRUPT) {
      delta = -delta;
    }

    return BCIndex{nextInstrOffset()} + delta;
  }

  BCOffset nextInstrOffset() const {
    return BCOffset{index() + inlineCacheSize(code_, index().value()) + 1};
  }

 private:
  _Py_CODEUNIT word() const {
    return codeUnit(code_)[index().value()];
  }

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
  explicit BytecodeInstructionBlock(BorrowedRef<PyCodeObject> code)
      : BytecodeInstructionBlock{
            code,
            BCIndex{0},
            BCIndex{countIndices(code)}} {}

  BytecodeInstructionBlock(
      BorrowedRef<PyCodeObject> code,
      BCIndex start,
      BCIndex end)
      : code_{Ref<PyCodeObject>::create(code)},
        start_idx_{start},
        end_idx_{end} {}

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

  Iterator begin() const {
    return Iterator{code_, start_idx_, end_idx_};
  }

  Iterator end() const {
    return Iterator{code_, end_idx_, end_idx_};
  }

  BCOffset startOffset() const {
    return start_idx_;
  }

  BCOffset endOffset() const {
    return end_idx_;
  }

  Py_ssize_t size() const {
    return end_idx_ - start_idx_;
  }

  BytecodeInstruction at(BCIndex idx) const {
    JIT_CHECK(
        idx >= start_idx_ && idx < end_idx_,
        "Invalid index {}, bytecode block is [{}, {})",
        idx,
        start_idx_,
        end_idx_);
    return BytecodeInstruction{code_, idx};
  }

  BytecodeInstruction lastInstr() const {
    JIT_CHECK(size() > 0, "Block has no instructions");
    return BytecodeInstruction(code_, end_idx_ - 1);
  }

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

 private:
  Ref<PyCodeObject> code_;
  BCIndex start_idx_;
  BCIndex end_idx_;
};

} // namespace jit
