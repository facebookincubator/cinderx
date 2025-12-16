// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

using jit::BCOffset;
using namespace jit::hir;

class FrameStateCreationTest : public RuntimeTest {};

TEST_F(FrameStateCreationTest, InitialInstrOffset) {
  FrameState frame;
  EXPECT_EQ(frame.cur_instr_offs.value(), -sizeof(_Py_CODEUNIT));
}

#define EXPECT_HIR_EQ(irfunc, expected)                                     \
  {                                                                         \
    ASSERT_TRUE(irfunc != nullptr);                                         \
    EXPECT_EQ(                                                              \
        HIRPrinter{}.setFullSnapshots(true).ToString(*(irfunc)), expected); \
  }

TEST_F(FrameStateCreationTest, LoadGlobal) {
  const char* src = R"(
def test():
  return foo
)";
  std::unique_ptr<Function> irfunc;
  CompileToHIR(src, "test", irfunc);
#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadCurrentFunc
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
    v3 = LoadGlobal<0; "foo"> {
      FrameState {
        CurInstrOffset 2
      }
    }
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
    Snapshot {
      CurInstrOffset 0
    }
    v0 = LoadGlobal<0; "foo"> {
      FrameState {
        CurInstrOffset 0
      }
    }
    Snapshot {
      CurInstrOffset 2
      Stack<1> v0
    }
    Return v0
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
    v5 = LoadGlobal<0; "xs"> {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
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
    v7 = LoadConst<Nullptr>
    v3 = Assign v6
    v4 = Assign v7
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v10 = LoadEvalBreaker
    CondBranch<8, 1> v10
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<2> v3 v4
    }
    v11 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<2> v3 v4
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<2> v3 v4
    }
    v8 = InvokeIterNext v3 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<2> v3 v4
      }
    }
    v5 = Assign v8
    CondBranchIterNotDone<2, 4> v5
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<3> v3 v4 v5
    }
    v1 = Assign v5
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<2> v3 v4
    }
    v9 = LoadConst<ImmortalNoneType>
    Return v9
  }
}
)";
#elif PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
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
    v5 = LoadGlobal<0; "xs"> {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
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
    v3 = Assign v6
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v9 = LoadEvalBreaker
    CondBranch<8, 1> v9
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v3
    }
    v10 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v3
    }
    v7 = InvokeIterNext v3 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    v4 = Assign v7
    CondBranchIterNotDone<2, 4> v4
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v3 v4
    }
    v1 = Assign v4
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 26
      Locals<2> v0 v1
      Stack<1> v3
    }
    v8 = LoadConst<ImmortalNoneType>
    Return v8
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    v2 = LoadCurrentFunc
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
    v5 = LoadGlobal<0; "xs"> {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
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
    v3 = Assign v6
    Branch<7>
  }

  bb 7 (preds 2, 5) {
    v9 = LoadEvalBreaker
    CondBranch<8, 1> v9
  }

  bb 8 (preds 7) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v3
    }
    v10 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    Branch<1>
  }

  bb 1 (preds 7, 8) {
    Snapshot {
      CurInstrOffset 14
      Locals<2> v0 v1
      Stack<1> v3
    }
    v7 = InvokeIterNext v3 {
      FrameState {
        CurInstrOffset 14
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    v4 = Assign v7
    CondBranchIterNotDone<2, 4> v4
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 18
      Locals<2> v0 v1
      Stack<2> v3 v4
    }
    v1 = Assign v4
    Branch<7>
  }

  bb 4 (preds 1) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
    }
    v8 = LoadConst<ImmortalNoneType>
    v8 = RefineType<ImmortalNoneType> v8
    Return<ImmortalNoneType> v8
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "fs">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v2 = LoadGlobal<0; "xs"> {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 2
      Locals<2> v0 v1
      Stack<1> v2
    }
    v3 = GetIter v2 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
    v2 = Assign v3
    Branch<4>
  }

  bb 4 (preds 0, 2) {
    v6 = LoadEvalBreaker
    CondBranch<5, 1> v6
  }

  bb 5 (preds 4) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
      Stack<1> v2
    }
    v7 = RunPeriodicTasks {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v2
      }
    }
    Branch<1>
  }

  bb 1 (preds 4, 5) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
      Stack<1> v2
    }
    v4 = InvokeIterNext v2 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v2
      }
    }
    v3 = Assign v4
    CondBranchIterNotDone<2, 3> v3
  }

  bb 2 (preds 1) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<2> v2 v3
    }
    v1 = Assign v3
    Branch<4>
  }

  bb 3 (preds 1) {
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
    }
    v5 = LoadConst<NoneType>
    Return v5
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

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v3 = Assign v0
    CondBranch<1, 2> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v0 v1
      Stack<1> v3
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v3 = Assign v0
    CondBranch<1, 2> v5
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v3
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v2 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v3 = Assign v0
    CondBranch<1, 2> v2
  }

  bb 1 (preds 0) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 0, 1) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
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

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v3 = Assign v0
    CondBranch<2, 1> v7
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 20
      Locals<2> v0 v1
      Stack<1> v3
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 24
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v3 = Assign v0
    CondBranch<2, 1> v5
  }

  bb 1 (preds 3) {
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v3
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 1, 3) {
    Snapshot {
      CurInstrOffset 12
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v2 = IsTruthy v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v3 = Assign v0
    CondBranch<2, 1> v2
  }

  bb 1 (preds 0) {
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    v3 = Assign v1
    Branch<2>
  }

  bb 2 (preds 0, 1) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"f"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"a"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = VectorCall<1> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v2
    }
    Return v2
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
#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadArg<1; "a">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"f"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v2 = LoadMethod<0; "bar"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
      }
    }
    v3 = GetSecondOutput<OptObject> v2
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
      Stack<2> v2 v3
    }
    v1 = CheckVar<"a"> v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v2 v3
      }
    }
    v4 = CallMethod<3> v2 v3 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v4
    }
    Return v4
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
#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    v1 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "f">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"f"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = LoadAttr<0; "a"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v1
    }
    v2 = LoadAttr<1; "b"> v1 {
      FrameState {
        CurInstrOffset 4
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = InPlaceOp<Xor> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v2
    }
    v0 = Assign v2
    v3 = LoadConst<NoneType>
    Return v3
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = BinaryOp<Add> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v2
    }
    Return v2
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
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
    v7 = LoadConst<ImmortalBool[False]>
    v6 = PrimitiveCompare<Equal> v7 v5
    v8 = PrimitiveBoxBool v6
    Snapshot {
      CurInstrOffset 14
      Locals<1> v0
      Stack<1> v8
    }
    Return v8
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = UnaryOp<Not> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v1
    }
    Return v1
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v1
      }
    }
    StoreAttr<0; "foo"> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
    }
    v2 = LoadConst<NoneType>
    Return v2
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v1
      }
    }
    v2 = LoadConst<ImmortalLongExact[1]>
    StoreSubscr v0 v2 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
    }
    v3 = LoadConst<NoneType>
    Return v3
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = LoadConst<MortalTupleExact[tuple:0xdeadbeef]>
    v3 = MakeDict<2> {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<3> v0 v1 v2
      }
    }
    v4 = LoadTupleItem<0> v2
    v5 = SetDictItem v3 v4 v0 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v6 = LoadTupleItem<1> v2
    v7 = SetDictItem v3 v6 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<2> v0 v1
      Stack<1> v3
    }
    Return v3
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v5 = MakeList<2> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v5 = MakeList<2> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
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
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = MakeList<2> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v2
    }
    Return v2
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v5 = MakeTuple<2> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v5
    }
    Return v5
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v1 = LoadArg<1; "y">
    v2 = LoadCurrentFunc
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
    v5 = MakeTuple<2> v0 v1 {
      FrameState {
        CurInstrOffset 6
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
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
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"y"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = MakeTuple<2> v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v2
    }
    Return v2
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
#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
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
    v5 = MakeTuple<1> v0 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
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
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    v2 = LoadCurrentFunc
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
    v5 = MakeTuple<1> v0 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
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
#else
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v0 = LoadArg<0; "x">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"x"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v2 = MakeTuple<1> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<2> v0 v1
      Stack<1> v2
    }
    v3 = LoadConst<MortalCode["foo"]>
    v4 = LoadConst<MortalUnicodeExact["test.<locals>.foo"]>
    v5 = MakeFunction v3 v4 {
      FrameState {
        CurInstrOffset 8
        Locals<2> v0 v1
        Stack<1> v2
      }
    }
    SetFunctionAttr<func_defaults> v2 v5
    Snapshot {
      CurInstrOffset 10
      Locals<2> v0 v1
      Stack<1> v5
    }
    v1 = Assign v5
    v1 = CheckVar<"foo"> v1 {
      FrameState {
        CurInstrOffset 12
        Locals<2> v0 v1
      }
    }
    Return v1
  }
}
)";
#endif
  EXPECT_HIR_EQ(irfunc, expected);
}

TEST_F(FrameStateCreationTest, GetDominatingFrameState) {
  CFG cfg;
  auto block = cfg.AllocateBlock();
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
