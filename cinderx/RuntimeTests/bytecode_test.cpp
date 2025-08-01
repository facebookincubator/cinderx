// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/RuntimeTests/fixtures.h"

using BytecodeInstructionIteratorTest = RuntimeTest;

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

  jit::BytecodeInstructionBlock bc_block{code};
  auto it = bc_block.begin();
  EXPECT_EQ(it->opcode(), LOAD_CONST);
  EXPECT_EQ(it->oparg(), 0x010203);

  auto it2 = it++;
  EXPECT_EQ(it2->opcode(), LOAD_CONST);
  EXPECT_EQ(it2->oparg(), 0x010203);
  EXPECT_EQ(it->opcode(), LOAD_CONST);
  EXPECT_EQ(it->oparg(), 0x0102);

  ++it;
  EXPECT_EQ(it, bc_block.end());
}
