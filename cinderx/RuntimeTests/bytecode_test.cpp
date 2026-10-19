// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace cinderx {

using BytecodeInstructionTest = RuntimeTest;
using BytecodeInstructionIteratorTest = RuntimeTest;

using namespace cinderx::jit;

// Verify that BytecodeInstruction won't crash when given an invalid offset.
TEST_F(BytecodeInstructionTest, OutOfBoundsOffset) {
  Ref<PyFunctionObject> func(compileStockAndGet("def f(): pass", "f"));

  for (int offset : {-2, 1'000'000}) {
    BytecodeInstruction instr{func->func_code, BCOffset{offset}};

    // Returns invalid opcodes and offsets.
    ASSERT_LT(instr.opcodeOffset(), 0);
    ASSERT_EQ(instr.opcode(), -1);
    ASSERT_EQ(instr.specializedOpcode(), -1);
    ASSERT_EQ(instr.oparg(), 0);

    ASSERT_FALSE(instr.isBranch());
    ASSERT_FALSE(instr.isBackwardBranch());
    ASSERT_FALSE(instr.isReturn());
    ASSERT_FALSE(instr.isTerminator());
  }
}

TEST_F(BytecodeInstructionIteratorTest, ConsumesExtendedArgs) {
  //  0  EXTENDED_ARG  1
  //  2  EXTENDED_ARG  2
  //  4  LOAD_CONST    3
  //  6  EXTENDED_ARG  1
  //  8  LOAD_CONST    2
  // clang (correctly) complains we can't narrow EXTENDED_ARG to `char`, so
  // we're forced to use unsigned char here and cast in the call to
  // PyBytes_FromStringAndSize.
  const unsigned char bc[] = {
      EXTENDED_ARG,
      1,
      EXTENDED_ARG,
      2,
      LOAD_CONST,
      3,
      EXTENDED_ARG,
      1,
      LOAD_CONST,
      2};
  auto bytecode = Ref<>::steal(
      PyBytes_FromStringAndSize(reinterpret_cast<const char*>(bc), sizeof(bc)));
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto consts = Ref<>::steal(PyTuple_New(1));
  Py_INCREF(Py_None);
  PyTuple_SET_ITEM(consts.get(), 0, Py_None);
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      consts,
      /*names=*/empty_tuple,
      /*varnames=*/empty_tuple,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  BytecodeInstructionBlock bc_block{code};
  auto it = bc_block.begin();
  EXPECT_FALSE(it.atEnd());
  EXPECT_EQ(it->opcode(), LOAD_CONST);
  EXPECT_EQ(it->oparg(), 0x010203);

  auto it2 = it++;
  EXPECT_EQ(it2->opcode(), LOAD_CONST);
  EXPECT_EQ(it2->oparg(), 0x010203);
  EXPECT_FALSE(it.atEnd());
  EXPECT_EQ(it->opcode(), LOAD_CONST);
  EXPECT_EQ(it->oparg(), 0x0102);

  ++it;
  EXPECT_EQ(it, bc_block.end());
  EXPECT_TRUE(it.atEnd());
  EXPECT_TRUE(bc_block.end().atEnd());
}

TEST_F(BytecodeInstructionIteratorTest, EmptyBlock) {
  Ref<PyFunctionObject> func(compileStockAndGet("def f(): pass", "f"));
  BorrowedRef<PyCodeObject> code{func->func_code};

  for (BCIndex index : {BCIndex{0}, BCIndex{countIndices(code)}}) {
    BytecodeInstructionBlock block{code, index, index};
    EXPECT_EQ(block.begin(), block.end());
    EXPECT_TRUE(block.begin().atEnd());
    EXPECT_TRUE(block.end().atEnd());
  }
}

TEST_F(BytecodeInstructionIteratorTest, RetainsOpcodeAcrossSpecialization) {
  Ref<PyFunctionObject> func(compileStockAndGet(
      "def f(iterable):\n"
      "    for item in iterable:\n"
      "        pass\n",
      "f"));
  BorrowedRef<PyCodeObject> code{func->func_code};
  BytecodeInstructionBlock block{code};
  for (const BytecodeInstruction& instr : block) {
    if (instr.opcode() != FOR_ITER) {
      continue;
    }
    const BCIndex index = instr.opcodeIndex();
    const _Py_CODEUNIT original = codeUnit(code)[index.value()];
    const BCOffset target = instr.getJumpTarget();
    const BCOffset next = instr.nextInstrOffset();
    for (int opcode : {FOR_ITER, FOR_ITER_RANGE}) {
      codeUnit(code)[index.value()] =
          _Py_MAKE_CODEUNIT(opcode, _Py_OPARG(original));
      ThreadedCompileContext context;
      context.releaseGil();
      BytecodeInstruction saved{code, index};
      BytecodeInstruction copy = saved;

      const int changed_opcode = opcode == FOR_ITER ? FOR_ITER_RANGE : FOR_ITER;
      {
        ThreadedCompileGILHolder gil;
        codeUnit(code)[index.value()] =
            _Py_MAKE_CODEUNIT(changed_opcode, _Py_OPARG(original) + 1);
      }

      EXPECT_EQ(saved.specializedOpcode(), opcode);
      EXPECT_EQ(copy.specializedOpcode(), opcode);
      EXPECT_EQ(saved.opcode(), FOR_ITER);
      EXPECT_EQ(copy.opcode(), FOR_ITER);
      EXPECT_EQ(saved.oparg(), _Py_OPARG(original));
      EXPECT_EQ(copy.oparg(), _Py_OPARG(original));
      EXPECT_EQ(saved.getJumpTarget(), target);
      EXPECT_EQ(copy.getJumpTarget(), target);
      EXPECT_EQ(saved.nextInstrOffset(), next);
      EXPECT_EQ(copy.nextInstrOffset(), next);
      EXPECT_EQ(
          (BytecodeInstruction{code, index}.specializedOpcode()),
          changed_opcode);
      EXPECT_EQ(PyThreadState_GetUnchecked(), nullptr);
    }
    codeUnit(code)[index.value()] = original;
    return;
  }
  FAIL() << "Expected a FOR_ITER instruction";
}

} // namespace cinderx
