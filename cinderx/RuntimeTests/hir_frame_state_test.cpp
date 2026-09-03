// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace cinderx {

using cinderx::jit::BCOffset;

using namespace cinderx::jit::hir;

class FrameStateCreationTest : public RuntimeTest {};

TEST_F(FrameStateCreationTest, InitialInstrOffset) {
  FrameState frame;
  EXPECT_EQ(frame.cur_instr_offs.value(), -sizeof(_Py_CODEUNIT));
}

#define EXPECT_HIR_EQ(irfunc, expected)                                     \
  {                                                                         \
    ASSERT_TRUE(irfunc != nullptr);                                         \
    EXPECT_EQ(                                                              \
        HIRPrinter{}.setFullSnapshots(true).toString(*(irfunc)), expected); \
  }

TEST_F(FrameStateCreationTest, LoadGlobal) {
  const char* src = R"(
def test():
  return foo
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadEvalBreaker
    CondBranch<2, 1> v1
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
    }
    v2 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
    }
    v3 = LoadGlobalCached<0; "foo">
    Guard v3 {
      FrameState {
        CurInstrOffset 4
      }
    }
    v3 = RefineType<Object> v3
    Snapshot {
      CurInstrOffset 14
      Stack<1> v3
    }
    Return v3
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    AtQuiescentState
    v1 = LoadEvalBreaker
    CondBranch<2, 1> v1
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
    }
    v2 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
    }
    v3 = LoadGlobalCached<0; "foo">
    Guard v3 {
      FrameState {
        CurInstrOffset 2
      }
    }
    v3 = RefineType<Object> v3
    Snapshot {
      CurInstrOffset 12
      Stack<1> v3
    }
    Return v3
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadEvalBreaker
    CondBranch<2, 1> v1
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
    }
    v2 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
    }
    v3 = LoadGlobalCached<0; "foo">
    Guard v3 {
      FrameState {
        CurInstrOffset 2
      }
    }
    v3 = RefineType<Object> v3
    Snapshot {
      CurInstrOffset 12
      Stack<1> v3
    }
    Return v3
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, GetIterForIter) {
  const char* src = R"(
def test(fs):
  for x in xs:
    pass
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<6, 5> v3
  }

  bb 6 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<5>
  }

  bb 5 (preds 0, 6) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = LoadGlobalCached<0; "xs">
    Guard v5 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    v5 = RefineType<Object> v5
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = GetIter v5 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
      }
    }
    v7 = LoadConst<Nullptr>
    v8 = Assign v6
    v9 = Assign v7
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v13 = LoadEvalBreaker
    CondBranch<8, 1> v13
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v8 v9
    }
    v14 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 18
        Locals<2> v0 v1
        Stack<2> v8 v9
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v8 v9
    }
    v10 = InvokeIterNext v8 {
      FrameState {
        CurInstrOffset 18
        Locals<2> v0 v1
        Stack<2> v8 v9
      }
    }
    v11 = Assign v10
    CondBranchIterNotDone<2, 4> v11
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 22
      Locals<2> v0 v1
      Stack<3> v8 v9 v11
    }
    v1 = Assign v11
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 30
      Locals<2> v0 v1
      Stack<2> v8 v9
    }
    v12 = LoadConst<ImmortalNoneType>
    Snapshot {
      CurInstrOffset 34
      Locals<2> v0 v1
      Stack<1> v12
    }
    Return v12
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v1
    }
    AtQuiescentState
    v4 = LoadEvalBreaker
    CondBranch<6, 5> v4
  }

  bb 6 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v1
    }
    v5 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v1
      }
    }
    Branch<5>
  }

  bb 5 (preds 0, 6) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v1
    }
    v6 = LoadGlobalCached<0; "xs">
    Guard v6 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v3 v1
      }
    }
    v6 = RefineType<Object> v6
    Snapshot {
      CurInstrOffset 12
      Locals<2> v3 v1
      Stack<1> v6
    }
    v7 = GetIter v6 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v3 v1
      }
    }
    v8 = Assign v7
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    AtQuiescentState
    v12 = LoadEvalBreaker
    CondBranch<8, 1> v12
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v3 v1
      Stack<1> v8
    }
    v13 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v3 v1
        Stack<1> v8
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v3 v1
      Stack<1> v8
    }
    v9 = InvokeIterNext v8 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v3 v1
        Stack<1> v8
      }
    }
    v10 = Assign v9
    CondBranchIterNotDone<2, 4> v10
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v3 v1
      Stack<2> v8 v10
    }
    v1 = Assign v10
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v3 v1
      Stack<1> v8
    }
    v11 = LoadConst<ImmortalNoneType>
    Return v11
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<6, 5> v3
  }

  bb 6 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<5>
  }

  bb 5 (preds 0, 6) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadGlobalCached<0; "xs">
    Guard v5 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
    v5 = RefineType<Object> v5
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = GetIter v5 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
      }
    }
    v7 = Assign v6
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v11 = LoadEvalBreaker
    CondBranch<8, 1> v11
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    v12 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v7
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    v8 = InvokeIterNext v7 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v7
      }
    }
    v9 = Assign v8
    CondBranchIterNotDone<2, 4> v9
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v7 v9
    }
    v1 = Assign v9
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<1> v7
    }
    v10 = LoadConst<ImmortalNoneType>
    Return v10
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<6, 5> v3
  }

  bb 6 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<5>
  }

  bb 5 (preds 0, 6) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadGlobalCached<0; "xs">
    Guard v5 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
    v5 = RefineType<Object> v5
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = GetIter v5 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
      }
    }
    v7 = Assign v6
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v11 = LoadEvalBreaker
    CondBranch<8, 1> v11
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    v12 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v7
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    v8 = InvokeIterNext v7 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v7
      }
    }
    v9 = Assign v8
    CondBranchIterNotDone<2, 4> v9
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v7 v9
    }
    v1 = Assign v9
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
    }
    v10 = LoadConst<ImmortalNoneType>
    v10 = RefineType<ImmortalNoneType> v10
    Return<ImmortalNoneType> v10
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, NonUniformConditionals1) {
  // This function has different operand stack contents along each branch of
  // the conditional
  const char* src = R"(
def test(x, y):
  return x and y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);

#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<4, 3> v5
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = IsTruthy v3 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v3 v4
        Stack<1> v3
      }
    }
    v8 = PrimitiveBoxBool v7
    Snapshot {
      CurInstrOffset 14
      Locals<2> v3 v4
      Stack<2> v3 v8
    }
    v10 = LoadConst<ImmortalBool[True]>
    v9 = PrimitiveCompare<Equal> v8 v10
    v11 = Assign v3
    CondBranch<1, 2> v9
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v3 v4
      Stack<1> v11
    }
    v11 = Assign v4
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v3 v4
      Stack<1> v11
    }
    Return v11
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = PrimitiveBoxBool v5
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<2> v0 v6
    }
    v8 = LoadConst<ImmortalBool[True]>
    v7 = PrimitiveCompare<Equal> v6 v8
    v9 = Assign v0
    CondBranch<1, 2> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 22
      Locals<2> v0 v1
      Stack<1> v9
    }
    v9 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<1> v9
    }
    Return v9
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = PrimitiveBoxBool v5
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<2> v0 v6
    }
    v8 = LoadConst<ImmortalBool[True]>
    v7 = PrimitiveCompare<Equal> v6 v8
    v9 = Assign v0
    CondBranch<1, 2> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v0 v1
      Stack<1> v9
    }
    v9 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
      Stack<1> v9
    }
    Return v9
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = Assign v0
    CondBranch<1, 2> v5
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v6
    }
    v6 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, NonUniformConditionals2) {
  // This function has different operand stack contents along each branch of
  // the conditional
  const char* src = R"(
def test(x, y):
  return x or y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);

#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<4, 3> v5
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = IsTruthy v3 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v3 v4
        Stack<1> v3
      }
    }
    v8 = PrimitiveBoxBool v7
    Snapshot {
      CurInstrOffset 14
      Locals<2> v3 v4
      Stack<2> v3 v8
    }
    v10 = LoadConst<ImmortalBool[True]>
    v9 = PrimitiveCompare<Equal> v8 v10
    v11 = Assign v3
    CondBranch<2, 1> v9
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v3 v4
      Stack<1> v11
    }
    v11 = Assign v4
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v3 v4
      Stack<1> v11
    }
    Return v11
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = PrimitiveBoxBool v5
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<2> v0 v6
    }
    v8 = LoadConst<ImmortalBool[True]>
    v7 = PrimitiveCompare<Equal> v6 v8
    v9 = Assign v0
    CondBranch<2, 1> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 22
      Locals<2> v0 v1
      Stack<1> v9
    }
    v9 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<1> v9
    }
    Return v9
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = PrimitiveBoxBool v5
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<2> v0 v6
    }
    v8 = LoadConst<ImmortalBool[True]>
    v7 = PrimitiveCompare<Equal> v6 v8
    v9 = Assign v0
    CondBranch<2, 1> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v0 v1
      Stack<1> v9
    }
    v9 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
      Stack<1> v9
    }
    Return v9
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<4, 3> v3
  }

  bb 4 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<3>
  }

  bb 3 (preds 0, 4) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v6 = Assign v0
    CondBranch<2, 1> v5
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v6
    }
    v6 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, CallFunction) {
  const char* src = R"(
def test(f, a):
  return f(a)
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = LoadConst<Nullptr>
    v8 = CallMethod<3> v3 v7 v4 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v3 v4
      Stack<1> v8
    }
    Return v8
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = LoadConst<Nullptr>
    v6 = CallMethod<3> v0 v5 v1 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<Nullptr>
    v6 = CallMethod<3> v0 v5 v1 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<Nullptr>
    v6 = CallMethod<3> v5 v0 v1 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, LoadCallMethod) {
  const char* src = R"(
def test(f, a):
  return f.bar(a)
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = LoadMethod<0; "bar"> v3 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
      }
    }
    v8 = GetSecondOutput<OptObject> v7
    Snapshot {
      CurInstrOffset 24
      Locals<2> v3 v4
      Stack<2> v7 v8
    }
    v9 = CallMethod<3> v7 v8 v4 {
      FrameState {
        CurInstrOffset 26
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 34
      Locals<2> v3 v4
      Stack<1> v9
    }
    Return v9
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = LoadMethod<0; "bar"> v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    v6 = GetSecondOutput<OptObject> v5
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<2> v5 v6
    }
    v7 = CallMethod<3> v5 v6 v1 {
      FrameState {
        CurInstrOffset 28
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 36
      Locals<2> v0 v1
      Stack<1> v7
    }
    Return v7
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadMethod<0; "bar"> v0 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    v6 = GetSecondOutput<OptObject> v5
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
      Stack<2> v5 v6
    }
    v7 = CallMethod<3> v5 v6 v1 {
      FrameState {
        CurInstrOffset 26
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 34
      Locals<2> v0 v1
      Stack<1> v7
    }
    Return v7
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, LoadAttr) {
  const char* src = R"(
def test(f):
  return f.a.b
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadCurrentFunc
    LoadFrame
    v2 = TagIfDeferred v0
    Snapshot {
      CurInstrOffset 0
      Locals<1> v2
    }
    AtQuiescentState
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v2
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v2
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<1> v2
    }
    v5 = LoadAttr<0; "a"> v2 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v2
      }
    }
    Snapshot {
      CurInstrOffset 24
      Locals<1> v2
      Stack<1> v5
    }
    v6 = LoadAttr<1; "b"> v5 {
      FrameState {
        CurInstrOffset 24
        Locals<1> v2
      }
    }
    Snapshot {
      CurInstrOffset 44
      Locals<1> v2
      Stack<1> v6
    }
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadEvalBreaker
    CondBranch<2, 1> v2
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v3 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
    }
    v4 = LoadAttr<0; "a"> v0 {
      FrameState {
        CurInstrOffset 6
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 26
      Locals<1> v0
      Stack<1> v4
    }
    v5 = LoadAttr<1; "b"> v4 {
      FrameState {
        CurInstrOffset 26
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 46
      Locals<1> v0
      Stack<1> v5
    }
    Return v5
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadEvalBreaker
    CondBranch<2, 1> v2
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v3 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<1> v0
    }
    v4 = LoadAttr<0; "a"> v0 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 24
      Locals<1> v0
      Stack<1> v4
    }
    v5 = LoadAttr<1; "b"> v4 {
      FrameState {
        CurInstrOffset 24
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 44
      Locals<1> v0
      Stack<1> v5
    }
    Return v5
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, InPlaceOp) {
  const char* src = R"(
def test(x, y):
  x ^= y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = InPlaceOp<Xor> v3 v4 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v3 v4
      Stack<1> v7
    }
    v3 = Assign v7
    v8 = LoadConst<ImmortalNoneType>
    Return v8
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = InPlaceOp<Xor> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<1> v5
    }
    v0 = Assign v5
    v6 = LoadConst<ImmortalNoneType>
    Snapshot {
      CurInstrOffset 22
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = InPlaceOp<Xor> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<1> v5
    }
    v0 = Assign v5
    v6 = LoadConst<ImmortalNoneType>
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = InPlaceOp<Xor> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<1> v5
    }
    v0 = Assign v5
    v6 = LoadConst<ImmortalNoneType>
    v6 = RefineType<ImmortalNoneType> v6
    Return<ImmortalNoneType> v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, BinaryOp) {
  const char* src = R"(
def test(x, y):
  return x + y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = BinaryOp<Add> v3 v4 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v3 v4
      Stack<1> v7
    }
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = BinaryOp<Add> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = BinaryOp<Add> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = BinaryOp<Add> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, UnaryOp) {
  const char* src = R"(
def test(x):
  return not x
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
    LoadFrame
    v2 = TagIfDeferred v0
    Snapshot {
      CurInstrOffset 0
      Locals<1> v2
    }
    AtQuiescentState
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v2
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v2
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<1> v2
    }
    v5 = IsTruthy v2 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v2
      }
    }
    v6 = PrimitiveBoxBool v5
    Snapshot {
      CurInstrOffset 12
      Locals<1> v2
      Stack<1> v6
    }
    v7 = UnaryOp<Not> v6 {
      FrameState {
        CurInstrOffset 12
        Locals<1> v2
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<1> v2
      Stack<1> v7
    }
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadEvalBreaker
    CondBranch<2, 1> v2
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v3 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
    }
    v4 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 6
        Locals<1> v0
      }
    }
    v5 = PrimitiveBoxBool v4
    Snapshot {
      CurInstrOffset 14
      Locals<1> v0
      Stack<1> v5
    }
    v6 = UnaryOp<Not> v5 {
      FrameState {
        CurInstrOffset 14
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<1> v0
      Stack<1> v6
    }
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadEvalBreaker
    CondBranch<2, 1> v2
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v3 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<1> v0
    }
    v4 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v0
      }
    }
    v5 = PrimitiveBoxBool v4
    Snapshot {
      CurInstrOffset 12
      Locals<1> v0
      Stack<1> v5
    }
    v6 = UnaryOp<Not> v5 {
      FrameState {
        CurInstrOffset 12
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<1> v0
      Stack<1> v6
    }
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadEvalBreaker
    CondBranch<2, 1> v2
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v3 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<1> v0
    }
    v4 = UnaryOp<Not> v0 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<1> v0
      Stack<1> v4
    }
    Return v4
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, StoreAttr) {
  const char* src = R"(
def test(x, y):
  x.foo = y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    StoreAttr<0; "foo"> v3 v4 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<2> v3 v4
    }
    v7 = LoadConst<ImmortalNoneType>
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    StoreAttr<0; "foo"> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalNoneType>
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    StoreAttr<0; "foo"> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalNoneType>
    Return v5
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    StoreAttr<0; "foo"> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalNoneType>
    v5 = RefineType<ImmortalNoneType> v5
    Return<ImmortalNoneType> v5
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, StoreSubscr) {
  const char* src = R"(
def test(x, y):
  x[1] = y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = LoadConst<ImmortalLongExact[1]>
    Snapshot {
      CurInstrOffset 6
      Locals<2> v3 v4
      Stack<3> v4 v3 v7
    }
    StoreSubscr v3 v7 v4 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v3 v4
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v3 v4
    }
    v8 = LoadConst<ImmortalNoneType>
    Return v8
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalLongExact[1]>
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<3> v1 v0 v5
    }
    StoreSubscr v0 v5 v1 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
    }
    v6 = LoadConst<ImmortalNoneType>
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalLongExact[1]>
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<3> v1 v0 v5
    }
    StoreSubscr v0 v5 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
    }
    v6 = LoadConst<ImmortalNoneType>
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalLongExact[1]>
    StoreSubscr v0 v5 v1 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
    }
    v6 = LoadConst<ImmortalNoneType>
    v6 = RefineType<ImmortalNoneType> v6
    Return<ImmortalNoneType> v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, DictLiteral) {
  const char* src = R"(
def test(x, y):
  return {'x': x, 'y': y}
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = LoadConst<ImmortalUnicodeExact["x"]>
    v8 = LoadConst<ImmortalUnicodeExact["y"]>
    v9 = MakeDict<2> {
      FrameState {
        CurInstrOffset 10
        Locals<2> v3 v4
        Stack<4> v7 v3 v8 v4
      }
    }
    v10 = SetDictItem v9 v7 v3 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v3 v4
        Stack<4> v7 v3 v8 v4
      }
    }
    v11 = SetDictItem v9 v8 v4 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v3 v4
        Stack<4> v7 v3 v8 v4
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<2> v3 v4
      Stack<1> v9
    }
    Return v9
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalUnicodeExact["x"]>
    v6 = LoadConst<ImmortalUnicodeExact["y"]>
    v7 = MakeDict<2> {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    v8 = SetDictItem v7 v5 v0 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    v9 = SetDictItem v7 v6 v1 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<ImmortalUnicodeExact["x"]>
    v6 = LoadConst<ImmortalUnicodeExact["y"]>
    v7 = MakeDict<2> {
      FrameState {
        CurInstrOffset 10
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    v8 = SetDictItem v7 v5 v0 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    v9 = SetDictItem v7 v6 v1 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v0 v1
        Stack<4> v5 v0 v6 v1
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v7
    }
    Return v7
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = LoadConst<MortalTupleExact[tuple:0xdeadbeef]>
    v6 = MakeDict<2> {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<3> v0 v1 v5
      }
    }
    v7 = LoadTupleItem<0> v5
    v8 = SetDictItem v6 v7 v0 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v9 = LoadTupleItem<1> v5
    v10 = SetDictItem v6 v9 v1 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<1> v6
    }
    Return v6
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, ListLiteral) {
  const char* src = R"(
def test(x, y):
  return [x, y]
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = MakeList<2> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
        Stack<2> v3 v4
      }
    }
    InitListElements<2> v7 v3 v4
    Snapshot {
      CurInstrOffset 6
      Locals<2> v3 v4
      Stack<1> v7
    }
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = MakeList<2> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitListElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeList<2> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitListElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeList<2> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitListElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, TupleLiteral) {
  const char* src = R"(
def test(x, y):
  return x, y
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    v4 = TagIfDeferred v1
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    AtQuiescentState
    v5 = LoadEvalBreaker
    CondBranch<2, 1> v5
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v4
    }
    v6 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v4
    }
    v7 = MakeTuple<2> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v4
        Stack<2> v3 v4
      }
    }
    InitTupleElements<2> v7 v3 v4
    Snapshot {
      CurInstrOffset 6
      Locals<2> v3 v4
      Stack<1> v7
    }
    Return v7
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = MakeTuple<2> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitTupleElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeTuple<2> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitTupleElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeTuple<2> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    InitTupleElements<2> v5 v0 v1
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, MakeFunction) {
  const char* src = R"(
def test(x):
  def foo(a=x):
    return a
  return foo
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
    LoadFrame
    v3 = TagIfDeferred v0
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v1
    }
    AtQuiescentState
    v4 = LoadEvalBreaker
    CondBranch<2, 1> v4
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v3 v1
    }
    v5 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v3 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v3 v1
    }
    v6 = MakeTuple<1> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v3 v1
        Stack<1> v3
      }
    }
    InitTupleElements<1> v6 v3
    Snapshot {
      CurInstrOffset 6
      Locals<2> v3 v1
      Stack<1> v6
    }
    v7 = LoadConst<MortalCode["foo"]>
    v9 = LoadConst<Nullptr>
    v8 = MakeFunction v7 v9 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v3 v1
        Stack<1> v6
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v3 v1
      Stack<2> v6 v8
    }
    SetFunctionAttr<func_defaults> v6 v8
    Snapshot {
      CurInstrOffset 12
      Locals<2> v3 v1
      Stack<1> v8
    }
    v1 = Assign v8
    Return v1
  }
}
)";
#elif PY_VERSION_HEX >= 0x030F0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v5 = MakeTuple<1> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    InitTupleElements<1> v5 v0
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = LoadConst<MortalCode["foo"]>
    v8 = LoadConst<Nullptr>
    v7 = MakeFunction v6 v8 {
      FrameState {
        CurInstrOffset 10
        Locals<2> v0 v1
        Stack<1> v5
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<2> v5 v7
    }
    SetFunctionAttr<func_defaults> v5 v7
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v7
    }
    v1 = Assign v7
    Return v1
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeTuple<1> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    InitTupleElements<1> v5 v0
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = LoadConst<MortalCode["foo"]>
    v8 = LoadConst<Nullptr>
    v7 = MakeFunction v6 v8 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<1> v5
      }
    }
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<2> v5 v7
    }
    SetFunctionAttr<func_defaults> v5 v7
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v7
    }
    v1 = Assign v7
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
    LoadFrame
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = LoadEvalBreaker
    CondBranch<2, 1> v3
  }

  bb 2 (preds 0) {
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v4 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Branch<1>
  }

  bb 1 (preds 0, 2) {
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
    }
    v5 = MakeTuple<1> {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    InitTupleElements<1> v5 v0
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    v6 = LoadConst<MortalCode["foo"]>
    v8 = LoadConst<Nullptr>
    v7 = MakeFunction v6 v8 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<1> v5
      }
    }
    SetFunctionAttr<func_defaults> v5 v7
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<1> v7
    }
    v1 = Assign v7
    Return v1
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, GetDominatingFrameState) {
  CFG cfg;
  auto block = cfg.allocateBlock();
  FrameState fs{BCOffset{10}};
  block->append<Snapshot>(fs);

  auto addCheckExc = [&block]() {
    return block->append<CheckExc>(nullptr, nullptr);
  };

  auto i1 = addCheckExc();
  auto i1_fs = i1->getDominatingFrameState();
  ASSERT_NE(i1_fs, nullptr);
  ASSERT_EQ(*i1_fs, fs);

  for (int i = 0; i < 5; i++) {
    addCheckExc();
  }
  auto i2 = addCheckExc();
  auto i2_fs = i2->getDominatingFrameState();
  ASSERT_NE(i2_fs, nullptr);
  ASSERT_EQ(*i2_fs, fs);
  FrameState fs2{BCOffset{20}};
  block->append<Snapshot>(fs2);

  for (int i = 0; i < 5; i++) {
    addCheckExc();
  }
  auto i3 = addCheckExc();
  auto i3_fs = i3->getDominatingFrameState();
  ASSERT_NE(i3_fs, nullptr);
  ASSERT_EQ(*i3_fs, fs2);
}

} // namespace cinderx
