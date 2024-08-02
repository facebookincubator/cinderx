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

// A structured, immutable representation of a CPython bytecode
class BytecodeInstruction {
 public:
  BytecodeInstruction(BorrowedRef<PyCodeObject> code, BCIndex idx)
      : BytecodeInstruction{codeUnit(code), idx} {}

  BytecodeInstruction(_Py_CODEUNIT* instrs, BCIndex idx) : offset_(idx) {
    _Py_CODEUNIT word = instrs[idx.value()];
    opcode_ = _Py_OPCODE(word);
    oparg_ = _Py_OPARG(word);
  }

  BytecodeInstruction(int opcode, int oparg, BCOffset offset)
      : offset_(offset), opcode_(opcode), oparg_(oparg) {}

  BCOffset offset() const {
    return offset_;
  }

  BCIndex index() const {
    return offset();
  }

  int opcode() const {
    return opcode_;
  }

  int oparg() const {
    return oparg_;
  }

  bool IsBranch() const {
    return kBranchOpcodes.count(opcode());
  }

  bool IsCondBranch() const {
    switch (opcode_) {
      case FOR_ITER:
      case POP_JUMP_IF_FALSE:
      case JUMP_IF_FALSE_OR_POP:
      case JUMP_IF_NONZERO_OR_POP:
      case JUMP_IF_TRUE_OR_POP:
      case JUMP_IF_ZERO_OR_POP: {
        return true;
      }
      default: {
        return false;
      }
    }
  }

  bool IsRaiseVarargs() const {
    return opcode() == RAISE_VARARGS;
  }

  bool IsReturn() const {
    return opcode() == RETURN_VALUE || opcode() == RETURN_PRIMITIVE;
  }

  bool IsTerminator() const {
    return IsBranch() || kBlockTerminatorOpcodes.count(opcode());
  }

  BCOffset GetJumpTarget() const {
    return GetJumpTargetAsIndex();
  }

  BCIndex GetJumpTargetAsIndex() const {
    JIT_DCHECK(
        IsBranch(),
        "calling GetJumpTargetAsIndex() on non-branch gives nonsense");
    if (kRelBranchOpcodes.count(opcode())) {
      return NextInstrIndex() + oparg();
    }
    return BCIndex{oparg()};
  }

  BCOffset NextInstrOffset() const {
    return NextInstrIndex();
  }

  BCIndex NextInstrIndex() const {
    return BCIndex{offset_} + 1;
  }

  void ExtendOpArgWith(int changes) {
    oparg_ = (changes << 8) | oparg_;
  }

 private:
  BCOffset offset_;
  int opcode_;
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
      : BytecodeInstructionBlock{code, BCIndex{0}, BCIndex{countInstrs(code)}} {
  }

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
          bci_{0, 0, BCOffset{0}} {
      if (!atEnd()) {
        // Iterator end() methods are supposed to be past the logical end of the
        // underlying data structure and should not be accessed
        // directly. Dereferencing the current instr would be a heap buffer
        // overflow.
        bci_ = BytecodeInstruction(currentOpcode(), currentOparg(), idx);
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
      idx_++;
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

    Py_ssize_t remainingInstrs() const {
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
        int opcode = currentOpcode();
        int oparg = (accum << 8) | currentOparg();
        bci_ = BytecodeInstruction(opcode, oparg, idx_);
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
        start_idx_ == 0,
        "Instructions can only be looked up by index when start_idx_ == 0");
    return BytecodeInstruction(code_, idx);
  }

  BytecodeInstruction lastInstr() const {
    return BytecodeInstruction(code_, end_idx_ - 1);
  }

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  _Py_CODEUNIT* bytecode() const {
    return codeUnit(code_);
  }

 private:
  Ref<PyCodeObject> code_;
  BCIndex start_idx_;
  BCIndex end_idx_;
};

} // namespace jit
