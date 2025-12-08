// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_intrinsics.h"
#endif

using namespace jit;
using namespace jit::hir;

HIRPrinter fullPrinter() {
  return HIRPrinter{}.setFullSnapshots(true);
}

TEST(BasicBlockTest, CanAppendInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);
  ASSERT_TRUE(block.GetTerminator()->IsReturn());
}

TEST(BasicBlockTest, CanIterateInstrs) {
  Environment env;
  BasicBlock block;
  auto v0 = env.AllocateRegister();
  block.append<LoadConst>(v0, TNoneType);
  block.append<Return>(v0);

  auto it = block.begin();
  ASSERT_TRUE(it->IsLoadConst());
  it++;
  ASSERT_TRUE(it->IsReturn());
  it++;
  ASSERT_TRUE(it == block.end());
}

TEST(BasicBlockTest, SplitAfterSplitsBlockAfterInstruction) {
  Environment env;
  CFG cfg;
  BasicBlock* head = cfg.AllocateBlock();
  auto v0 = env.AllocateRegister();
  head->append<LoadConst>(v0, TNoneType);
  Instr* load_const = head->GetTerminator();
  head->append<Return>(v0);
  BasicBlock* tail = cfg.splitAfter(*load_const);
  ASSERT_NE(nullptr, head->GetTerminator());
  EXPECT_TRUE(head->GetTerminator()->IsLoadConst());
  ASSERT_NE(nullptr, tail->GetTerminator());
  EXPECT_TRUE(tail->GetTerminator()->IsReturn());
}

TEST(CFGIterTest, IteratingEmptyCFGReturnsEmptyTraversal) {
  CFG cfg;
  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 0);
}

TEST(CFGIterTest, IteratingSingleBlockCFGReturnsOneBlock) {
  Environment env;
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // Add a single instuction to the block
  block->append<Return>(env.AllocateRegister());

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsBlocksOnlyOnce) {
  CFG cfg;
  BasicBlock* block = cfg.AllocateBlock();
  cfg.entry_block = block;

  // The block loops on itself
  block->append<Branch>(block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 1) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], block) << "Incorrect block returned";
}

TEST(CFGIterTest, VisitsAllBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* cond = cfg.AllocateBlock();
  cfg.entry_block = cond;

  BasicBlock* true_block = cfg.AllocateBlock();
  true_block->append<Return>(env.AllocateRegister());

  BasicBlock* false_block = cfg.AllocateBlock();
  false_block->append<Return>(env.AllocateRegister());

  cond->append<CondBranch>(env.AllocateRegister(), true_block, false_block);

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 3) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], cond) << "Should have visited cond block first";
  ASSERT_EQ(traversal[1], true_block)
      << "Should have visited true block second";
  ASSERT_EQ(traversal[2], false_block)
      << "Should have visited false block last";
}

TEST(CFGIterTest, VisitsLoops) {
  Environment env;
  CFG cfg;

  // Create the else block
  BasicBlock* outer_else = cfg.AllocateBlock();
  outer_else->append<Return>(env.AllocateRegister());

  // Create the inner loop
  BasicBlock* loop_cond = cfg.AllocateBlock();
  BasicBlock* loop_body = cfg.AllocateBlock();
  loop_body->append<Branch>(loop_cond);
  loop_cond->append<CondBranch>(env.AllocateRegister(), loop_body, outer_else);

  // Create the outer conditional
  BasicBlock* outer_cond = cfg.AllocateBlock();
  outer_cond->append<CondBranch>(env.AllocateRegister(), loop_cond, outer_else);
  cfg.entry_block = outer_cond;

  std::vector<BasicBlock*> traversal = cfg.GetRPOTraversal();
  ASSERT_EQ(traversal.size(), 4) << "Incorrect number of blocks returned";
  ASSERT_EQ(traversal[0], outer_cond) << "Should have visited outer cond first";
  ASSERT_EQ(traversal[1], loop_cond) << "Should have visited loop cond second";
  ASSERT_EQ(traversal[2], loop_body) << "Should have visited loop body third";
  ASSERT_EQ(traversal[3], outer_else) << "Should have visited else block last";
}

TEST(SplitCriticalEdgesTest, SplitsCriticalEdges) {
  auto hir_source = R"(
fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 2> v0
  }
  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<2>
  }
  bb 2 {
    v2 = Phi<0, 1> v0 v1
    CondBranch<3, 5> v2
  }
  bb 3 {
    Branch<5>
  }
  bb 5 {
    Return v2
  }
}
)";
  auto func = HIRParser{}.ParseHIR(hir_source);
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cout));

  func->cfg.splitCriticalEdges();
  const char* expected_hir = R"(fun test {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 5> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<2>
  }

  bb 5 (preds 0) {
    Branch<2>
  }

  bb 2 (preds 1, 5) {
    v2 = Phi<1, 5> v1 v0
    CondBranch<3, 6> v2
  }

  bb 3 (preds 2) {
    Branch<5>
  }

  bb 6 (preds 2) {
    Branch<5>
  }

  bb 5 (preds 3, 6) {
    Return v2
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected_hir);
}

TEST(RemoveTrampolineBlocksTest, DoesntModifySingleBlockLoops) {
  CFG cfg;
  Environment env;

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 0 (preds 0) {
  Branch<0>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesSimpleLoops) {
  CFG cfg;
  Environment env;

  auto t1 = cfg.AllocateBlock();
  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t1);
  t1->append<Branch>(cfg.entry_block);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 1 (preds 1) {
  Branch<1>
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, RemovesSimpleChain) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that looks like
  //
  // entry -> t2 -> t1 -> exit
  //
  // after removing tramponline blocks we should be left
  // with only the exit block
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(env.AllocateRegister());

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<Branch>(t2);

  removeTrampolineBlocks(&cfg);

  auto s = HIRPrinter().ToString(cfg);
  auto expected = R"(bb 0 {
  Return v0
}
)";
  ASSERT_EQ(s, expected);
}

TEST(RemoveTrampolineBlocksTest, ReducesLoops) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        1->2->3->4-+
  //                                 ^       |
  //                                 |       |
  //                                 +-------+
  //
  // the loop of trampoline blocks on the right should be
  // reduced to a single block that loops back on itself:
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //  exit                        4--+
  //                              ^  |
  //                              |  |
  //                              +--+
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  auto t2 = cfg.AllocateBlock();
  auto t3 = cfg.AllocateBlock();
  auto t4 = cfg.AllocateBlock();
  t1->append<Branch>(t2);
  t2->append<Branch>(t3);
  t3->append<Branch>(t4);
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, exit_block, t1);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  CondBranch<0, 4> v0
}

bb 0 (preds 5) {
  Return v0
}

bb 4 (preds 4, 5) {
  Branch<4>
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveTrampolineBlocksTest, UpdatesAllPredecessors) {
  CFG cfg;
  Environment env;

  // This constructs a CFG that look like
  //
  //              entry
  //                |
  //   +--- true ---+--- false ---+
  //   |                          |
  //   4                          3
  //   |                          |
  //   +----------->2<------------+
  //                |
  //                v
  //                1
  //                |
  //                v
  //               exit
  //
  // After removing trampoline blocks this should look like
  //
  //              entry
  //                |
  //                v
  //               exit
  Register* v0 = env.AllocateRegister();
  auto exit_block = cfg.AllocateBlock();
  exit_block->append<Return>(v0);

  auto t1 = cfg.AllocateBlock();
  t1->append<Branch>(exit_block);

  auto t2 = cfg.AllocateBlock();
  t2->append<Branch>(t1);

  auto t3 = cfg.AllocateBlock();
  t3->append<Branch>(t2);

  auto t4 = cfg.AllocateBlock();
  t4->append<Branch>(t2);

  cfg.entry_block = cfg.AllocateBlock();
  cfg.entry_block->append<CondBranch>(v0, t4, t3);

  removeTrampolineBlocks(&cfg);

  auto after = HIRPrinter().ToString(cfg);
  const char* expected = R"(bb 5 {
  Branch<0>
}

bb 0 (preds 5) {
  Return v0
}
)";
  ASSERT_EQ(after, expected);
}

TEST(RemoveUnreachableBlocks, RemovesTransitivelyUnreachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    Branch<1>
  }

  bb 2 {
    Branch<2>
  }

  bb 3 {
    Branch<2>
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Return v0
  }

  bb 12 {
    Branch<11>
  }

  bb 11 {
    v1 = LoadConst<NoneType>
    Return v1
  }

  bb 4 {
    Branch<2>
  }

  bb 10 {
    Branch<1>
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    Branch<1>
  }

  bb 1 (preds 0) {
    v0 = LoadConst<NoneType>
    Return v0
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

TEST(RemoveUnreachableBlocks, FixesPhisOfReachableBlocks) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 2 {
    v2 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 {
    v3 = Phi<0, 1, 2> v0 v1 v2
    Return v3
  }
}
)";

  std::unique_ptr<Function> func = HIRParser{}.ParseHIR(hir);
  ASSERT_NE(func, nullptr);

  removeUnreachableBlocks(*func);

  const char* expected = R"(fun foo {
  bb 0 {
    v0 = LoadConst<NoneType>
    CondBranch<1, 3> v0
  }

  bb 1 (preds 0) {
    v1 = LoadConst<NoneType>
    Branch<3>
  }

  bb 3 (preds 0, 1) {
    v3 = Phi<0, 1> v0 v1
    Return v3
  }
}
)";
  EXPECT_EQ(HIRPrinter{}.ToString(*func), expected);
}

template <class T>
Ref<> toByteString(T&& data) {
  auto sp = std::span{data};
  return Ref<>::steal(PyBytes_FromStringAndSize(
      reinterpret_cast<const char*>(sp.data()), sp.size_bytes()));
}

class HIRBuildTest : public RuntimeTest {
 public:
  template <class T>
  std::unique_ptr<Function> build_test(
      T&& bc,
      const std::vector<PyObject*>& locals /* borrowed */) {
    Ref<> bytecode = toByteString(std::span{std::forward<T>(bc)});
    assert(bytecode.get());
    const int nlocals = locals.size();

    auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
    auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
    auto consts = Ref<>::steal(PyTuple_New(nlocals));
    auto varnames = Ref<>::steal(PyTuple_New(nlocals));
    for (int i = 0; i < nlocals; i++) {
      PyObject* local = locals.at(i);
      Py_INCREF(local);
      PyTuple_SET_ITEM(consts.get(), i, local);
      PyTuple_SET_ITEM(
          varnames.get(),
          i,
          PyUnicode_FromString(fmt::format("param{}", i).c_str()));
    }

    auto empty_tuple = Ref<>::steal(PyTuple_New(0));
    auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
    auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
        /*argcount=*/1,
        /*kwonlyargcount*/ 0,
        /*nlocals=*/nlocals,
        /*stacksize=*/0,
        /*flags=*/0,
        bytecode,
        consts,
        /*names=*/empty_tuple,
        varnames,
        /*freevars=*/empty_tuple,
        /*cellvars=*/empty_tuple,
        filename,
        funcname,
        /*_unused_qualname=*/funcname,
        /*firstlineno=*/0,
        /*linetable=*/empty_bytes,
        /*_unused_exceptiontable=*/empty_bytes));
    assert(code != nullptr);

    auto func =
        Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
    assert(func != nullptr);

    return buildHIR(func);
  }
};

TEST_F(HIRBuildTest, GetLength) {
  //  0 LOAD_FAST  0
  //  2 GET_LENGTH
  //  4 RETURN_VALUE
  uint8_t bc[] = {LOAD_FAST, 0, GET_LEN, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = GetLength v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v2
    }
    v3 = Assign v2
    v2 = Assign v0
    Return v3
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = GetLength v0 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v0
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v1
    }
    v2 = Assign v1
    v1 = Assign v0
    Return v2
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

#if PY_VERSION_HEX < 0x030E0000
TEST_F(HIRBuildTest, LoadAssertionError) {
  // No LOAD_ASSERTION_ERROR on 3.14 and later
  //  0 LOAD_ASSERTION_ERROR
  //  2 RETURN_VALUE
  uint8_t bc[] = {LOAD_ASSERTION_ERROR, 0, RETURN_VALUE, 0};
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/0,
      /*kwonlyargcount=*/0,
      /*nlocals=*/0,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
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

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalTypeExact[AssertionError:obj]>
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
    }
    v0 = LoadConst<ImmortalTypeExact[AssertionError:obj]>
    Return v0
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, SetUpdate) {
  //  0 LOAD_FAST    0
  //  2 LOAD_FAST    1
  //  4 LOAD_FAST    2
  //  6 SET_UPDATE   1
  //  8 ROT_TWO
  //  10 POP_TOP
  //  12 RETURN_VALUE
  uint8_t bc[] = {
      LOAD_FAST,
      0,
      LOAD_FAST,
      1,
      LOAD_FAST,
      2,
      SET_UPDATE,
      1,

#if PY_VERSION_HEX < 0x030B0000
      ROT_TWO,
      0,
#else
      SWAP,
      2,
#endif
      POP_TOP,
      0,
      RETURN_VALUE,
      0,
  };
  Ref<> bytecode = toByteString(bc);
  ASSERT_NE(bytecode.get(), nullptr);
  auto filename = Ref<>::steal(PyUnicode_FromString("filename"));
  auto funcname = Ref<>::steal(PyUnicode_FromString("funcname"));
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto param0 = Ref<>::steal(PyUnicode_FromString("param0"));
  auto param1 = Ref<>::steal(PyUnicode_FromString("param1"));
  auto param2 = Ref<>::steal(PyUnicode_FromString("param2"));
  auto varnames =
      Ref<>::steal(PyTuple_Pack(3, param0.get(), param1.get(), param2.get()));
  auto empty_bytes = Ref<>::steal(PyBytes_FromString(""));
  auto code = Ref<PyCodeObject>::steal(PyUnstable_Code_New(
      /*argcount=*/3,
      /*kwonlyargcount=*/0,
      /*nlocals=*/3,
      /*stacksize=*/0,
      /*flags=*/0,
      bytecode,
      /*consts=*/empty_tuple,
      /*names=*/empty_tuple,
      varnames,
      /*freevars=*/empty_tuple,
      /*cellvars=*/empty_tuple,
      filename,
      funcname,
      /*_unused_qualname=*/funcname,
      /*firstlineno=*/0,
      /*linetable=*/empty_bytes,
      /*_unused_exceptiontable=*/empty_bytes));
  ASSERT_NE(code.get(), nullptr);

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadArg<1; "param1">
    v2 = LoadArg<2; "param2">
    v3 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<3> v0 v1 v2
    }
    v4 = SetUpdate v1 v2 {
      FrameState {
        CurInstrOffset 6
        Locals<3> v0 v1 v2
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<3> v0 v1 v2
      Stack<2> v0 v1
    }
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadArg<1; "param1">
    v2 = LoadArg<2; "param2">
    Snapshot {
      CurInstrOffset 0
      Locals<3> v0 v1 v2
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<3> v0 v1 v2
      }
    }
    v1 = CheckVar<"param1"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<3> v0 v1 v2
        Stack<1> v0
      }
    }
    v2 = CheckVar<"param2"> v2 {
      FrameState {
        CurInstrOffset 4
        Locals<3> v0 v1 v2
        Stack<2> v0 v1
      }
    }
    v3 = SetUpdate v1 v2 {
      FrameState {
        CurInstrOffset 6
        Locals<3> v0 v1 v2
        Stack<2> v0 v1
      }
    }
    Snapshot {
      CurInstrOffset 8
      Locals<3> v0 v1 v2
      Stack<2> v0 v1
    }
    Return v1
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

class EdgeCaseTest : public RuntimeTest {};

TEST_F(EdgeCaseTest, IgnoreUnreachableLoops) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
#if PY_VERSION_HEX < 0x030C0000
      JUMP_ABSOLUTE,
      4,
#else
      JUMP_BACKWARD,
      2,
#if PY_VERSION_HEX >= 0x030E0000
      // inline-cache slot for 3.14+
      0,
      0
#endif
#endif
  };
  Ref<> bytecode = toByteString(bc);
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

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
    }
    v0 = LoadConst<NoneType>
    Return v0
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(EdgeCaseTest, JumpBackwardNoInterrupt) {
  //  0 LOAD_CONST    0
  //  2 RETURN_VALUE
  //
  //  4 LOAD_CONST    0
  //  6 RETURN_VALUE
  //  8 JUMP_ABSOLUTE 4
  uint8_t bc[] = {
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
      LOAD_CONST,
      0,
      RETURN_VALUE,
      0,
#if PY_VERSION_HEX < 0x030C0000
      JUMP_ABSOLUTE,
      4,
#else
      JUMP_BACKWARD_NO_INTERRUPT,
      2,
#endif
  };
  Ref<> bytecode = toByteString(bc);
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

  auto func = Ref<PyFunctionObject>::steal(PyFunction_New(code, MakeGlobals()));
  ASSERT_NE(func.get(), nullptr);

  std::unique_ptr<Function> irfunc(buildHIR(func));
  ASSERT_NE(irfunc.get(), nullptr);
#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
    }
    v1 = LoadConst<ImmortalNoneType>
    Return v1
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
    }
    v0 = LoadConst<NoneType>
    Return v0
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

class CppInlinerTest : public RuntimeTest {};

TEST_F(CppInlinerTest, ChangingCalleeFunctionCodeCausesDeopt) {
  const char* pycode = R"(
def other():
  return 2

other_code = other.__code__

def g():
  return 1

def f():
  return g()
)";
  // Compile f
  Ref<PyObject> pyfunc(compileAndGet(pycode, "f"));
  ASSERT_NE(pyfunc, nullptr) << "Failed compiling func";
  // Call f
  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto call_result1 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result1, 1));
  // Set __code__
  Ref<PyObject> other_code(getGlobal("other_code"));
  ASSERT_NE(other_code, nullptr) << "Failed to get other_code global";
  int result = PyObject_SetAttrString(pyfunc, "__code__", other_code);
  ASSERT_NE(result, -1) << "Failed to set __code__";
  // Call f again
  auto call_result2 =
      Ref<>::steal(PyObject_Call(pyfunc, empty_tuple, /*kwargs=*/nullptr));
  EXPECT_TRUE(isIntEquals(call_result2, 2));
}

class HIRCloneTest : public RuntimeTest {};

TEST_F(HIRCloneTest, CanCloneInstrs) {
  Environment env;
  auto v0 = env.AllocateRegister();
  std::unique_ptr<Instr> load_const(
      LoadConst::create(v0, Type::fromObject(Py_False)));
  std::unique_ptr<Instr> new_load(load_const->clone());
  ASSERT_TRUE(new_load->IsLoadConst());
  EXPECT_TRUE(
      static_cast<LoadConst*>(new_load.get())->type() ==
      static_cast<LoadConst*>(load_const.get())->type());
  EXPECT_NE(load_const, new_load);
  EXPECT_EQ(load_const->output()->instr(), load_const.get());
  EXPECT_EQ(new_load->output()->instr(), load_const.get());
}

TEST_F(HIRCloneTest, CanCloneBranches) {
  Environment env;
  CFG cfg;
  BasicBlock* from = cfg.AllocateBlock();
  BasicBlock* to = cfg.AllocateBlock();
  cfg.entry_block = from;
  from->append<Branch>(to);
  Instr* branch = from->GetTerminator();
  std::unique_ptr<Instr> new_branch(branch->clone());
  ASSERT_TRUE(new_branch->IsBranch());
  EXPECT_EQ(branch->block(), from);
  EXPECT_EQ(new_branch->block(), nullptr);

  Edge* orig_edge = static_cast<Branch*>(branch)->edge(0);
  // Make sure that the two edges are different pointers with the same fields
  Edge* dup_edge = static_cast<Branch*>(new_branch.get())->edge(0);
  EXPECT_NE(orig_edge, dup_edge);

  EXPECT_EQ(orig_edge->from(), dup_edge->from());
  EXPECT_TRUE(from->out_edges().contains(orig_edge));
  EXPECT_TRUE(from->out_edges().contains(dup_edge));

  EXPECT_EQ(orig_edge->to(), dup_edge->to());
  EXPECT_TRUE(to->in_edges().contains(orig_edge));
  EXPECT_TRUE(to->in_edges().contains(dup_edge));
}

TEST_F(HIRCloneTest, CanCloneBorrwedRefFields) {
  Environment env;
  auto v0 = env.AllocateRegister();
  auto name = Ref<>::steal(PyUnicode_FromString("test"));
  std::unique_ptr<Instr> check(CheckVar::create(v0, v0, name));
  std::unique_ptr<Instr> new_check(check->clone());
  ASSERT_TRUE(new_check->IsCheckVar());
  BorrowedRef<> orig_name = static_cast<CheckVar*>(check.get())->name();
  BorrowedRef<> dup_name = static_cast<CheckVar*>(new_check.get())->name();
  EXPECT_EQ(orig_name, dup_name);
}

TEST_F(HIRCloneTest, CanCloneVariadicOpInstr) {
  Environment env;
  auto out = env.AllocateRegister();
  auto v0 = env.AllocateRegister();

  // Create a CallStatic with no arguments
  std::unique_ptr<Instr> call_static_no_args(
      CallStatic::create(0, out, nullptr, Type::fromObject(Py_None)));
  std::unique_ptr<Instr> new_call_static_no_args(call_static_no_args->clone());
  ASSERT_NE(call_static_no_args.get(), new_call_static_no_args.get());
  ASSERT_TRUE(new_call_static_no_args->IsCallStatic());

  CallStatic* orig_call = static_cast<CallStatic*>(call_static_no_args.get());
  CallStatic* dup_call =
      static_cast<CallStatic*>(new_call_static_no_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());

  // Create a CallStatic with one argument
  std::unique_ptr<Instr> call_static_one_arg(
      CallStatic::create(1, out, nullptr, Type::fromObject(Py_None), v0));
  std::unique_ptr<Instr> new_call_static_one_arg(call_static_one_arg->clone());
  ASSERT_NE(call_static_one_arg.get(), new_call_static_one_arg.get());
  ASSERT_TRUE(new_call_static_one_arg->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_one_arg.get());
  dup_call = static_cast<CallStatic*>(new_call_static_one_arg.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));

  // Create a CallStatic with two arguments
  std::unique_ptr<Instr> call_static_two_args(
      CallStatic::create(2, out, nullptr, Type::fromObject(Py_None), v0, v0));
  std::unique_ptr<Instr> new_call_static_two_args(
      call_static_two_args->clone());
  ASSERT_NE(call_static_two_args.get(), new_call_static_two_args.get());
  ASSERT_TRUE(new_call_static_two_args->IsCallStatic());

  orig_call = static_cast<CallStatic*>(call_static_two_args.get());
  dup_call = static_cast<CallStatic*>(new_call_static_two_args.get());
  EXPECT_EQ(orig_call->addr(), dup_call->addr());
  EXPECT_EQ(orig_call->ret_type(), dup_call->ret_type());
  EXPECT_EQ(orig_call->GetOperand(0), dup_call->GetOperand(0));
  EXPECT_EQ(orig_call->GetOperand(1), dup_call->GetOperand(1));
}

TEST_F(HIRCloneTest, CanCloneDeoptBase) {
  const char* hir = R"(fun jittestmodule:test {
  bb 0 {
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v1 = LoadConst<ImmortalLongExact[1]>
    v0 = Assign v1
    v2 = LoadGlobal<0; "foo"> {
      FrameState {
        CurInstrOffset 6
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
  auto irfunc = HIRParser().ParseHIR(hir);
  ASSERT_NE(irfunc, nullptr);
  ASSERT_TRUE(checkFunc(*irfunc, std::cout));
  reflowTypes(*irfunc);
  RefcountInsertion().Run(*irfunc);
  const char* expected = R"(fun jittestmodule:test {
  bb 0 {
    v1:ImmortalLongExact[1] = LoadConst<ImmortalLongExact[1]>
    v2:Object = LoadGlobal<0> {
      LiveValues<1> unc:v1
      FrameState {
        CurInstrOffset 6
        Locals<1> v1
      }
    }
    Return v2
  }
}
)";
  ASSERT_EQ(fullPrinter().ToString(*irfunc), expected);
  BasicBlock* bb0 = irfunc->cfg.entry_block;
  Instr& load_global = *(++(bb0->rbegin()));
  ASSERT_TRUE(load_global.IsLoadGlobal());

  std::unique_ptr<Instr> dup_load(load_global.clone());
  ASSERT_TRUE(dup_load->IsLoadGlobal());

  LoadGlobal* orig = static_cast<LoadGlobal*>(&load_global);
  LoadGlobal* dup = static_cast<LoadGlobal*>(dup_load.get());

  EXPECT_EQ(orig->output(), dup->output());
  EXPECT_EQ(orig->name_idx(), dup->name_idx());

  FrameState* orig_fs = orig->frameState();
  FrameState* dup_fs = dup->frameState();
  // Should not be pointer equal, but have equal contents
  EXPECT_NE(orig_fs, dup_fs);
  EXPECT_TRUE(*orig_fs == *dup_fs);

  // Should have equal contents
  EXPECT_TRUE(orig->live_regs() == dup->live_regs());
}

// ROT_N was removed in 3.11.
#if PY_VERSION_HEX < 0x030B0000
TEST_F(HIRBuildTest, ROT_N) {
  uint8_t bc[] = {
      LOAD_FAST,
      0,
      LOAD_FAST,
      1,
      LOAD_FAST,
      2,
      LOAD_FAST,
      3,
      ROT_N,
      3,
      BINARY_OR,
      0,
      BINARY_OR,
      0,
      BINARY_OR,
      0,
      RETURN_VALUE,
      0};

  std::unique_ptr<Function> irfunc =
      build_test(bc, {Py_None, Py_None, Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<4> v0 v1 v2 v3
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<4> v0 v1 v2 v3
      }
    }
    v1 = CheckVar<"param1"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<4> v0 v1 v2 v3
        Stack<1> v0
      }
    }
    v2 = CheckVar<"param2"> v2 {
      FrameState {
        CurInstrOffset 4
        Locals<4> v0 v1 v2 v3
        Stack<2> v0 v1
      }
    }
    v3 = CheckVar<"param3"> v3 {
      FrameState {
        CurInstrOffset 6
        Locals<4> v0 v1 v2 v3
        Stack<3> v0 v1 v2
      }
    }
    v4 = BinaryOp<Or> v1 v2 {
      FrameState {
        CurInstrOffset 10
        Locals<4> v0 v1 v2 v3
        Stack<2> v0 v3
      }
    }
    Snapshot {
      CurInstrOffset 12
      Locals<4> v0 v1 v2 v3
      Stack<3> v0 v3 v4
    }
    v5 = BinaryOp<Or> v3 v4 {
      FrameState {
        CurInstrOffset 12
        Locals<4> v0 v1 v2 v3
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 14
      Locals<4> v0 v1 v2 v3
      Stack<2> v0 v5
    }
    v6 = BinaryOp<Or> v0 v5 {
      FrameState {
        CurInstrOffset 14
        Locals<4> v0 v1 v2 v3
      }
    }
    Snapshot {
      CurInstrOffset 16
      Locals<4> v0 v1 v2 v3
      Stack<1> v6
    }
    Return v6
  }
}
)";

  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}
#endif

TEST_F(HIRBuildTest, MatchMapping) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_MAPPING, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[64]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = LoadField<ob_type@8, Type, borrowed> v0
    v2 = LoadField<tp_flags@168, CUInt64, borrowed> v1
    v3 = LoadConst<CUInt64[64]>
    v4 = IntBinaryOp<And> v2 v3
    CondBranch<1, 2> v4
  }

  bb 1 (preds 0) {
    v5 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v5 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v5
    }
    v1 = Assign v0
    Return v5
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, MatchSequence) {
  uint8_t bc[] = {LOAD_FAST, 0, MATCH_SEQUENCE, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = LoadField<ob_type@8, Type, borrowed> v0
    v3 = LoadField<tp_flags@168, CUInt64, borrowed> v2
    v4 = LoadConst<CUInt64[32]>
    v5 = IntBinaryOp<And> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v6 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v6 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v6
    }
    v2 = Assign v0
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = LoadField<ob_type@8, Type, borrowed> v0
    v2 = LoadField<tp_flags@168, CUInt64, borrowed> v1
    v3 = LoadConst<CUInt64[32]>
    v4 = IntBinaryOp<And> v2 v3
    CondBranch<1, 2> v4
  }

  bb 1 (preds 0) {
    v5 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v5 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<2> v0 v5
    }
    v1 = Assign v0
    Return v5
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, MatchKeys) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, MATCH_KEYS, 0, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = MatchKeys v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v4 = LoadConst<ImmortalNoneType>
    v5 = PrimitiveCompare<Equal> v3 v4
    CondBranch<1, 2> v5
  }

  bb 1 (preds 0) {
    v3 = RefineType<NoneType> v3
    Branch<3>
  }

  bb 2 (preds 0) {
    v3 = RefineType<TupleExact> v3
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<3> v0 v1 v3
    }
    v6 = Assign v3
    v3 = Assign v0
    v4 = Assign v1
    Return v6
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"param1"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = MatchKeys v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<2> v0 v1
      }
    }
    v3 = LoadConst<NoneType>
    v4 = PrimitiveCompare<Equal> v2 v3
    CondBranch<1, 2> v4
  }

  bb 1 (preds 0) {
    v2 = RefineType<NoneType> v2
    v5 = LoadConst<ImmortalBool[False]>
    Branch<3>
  }

  bb 2 (preds 0) {
    v2 = RefineType<TupleExact> v2
    v5 = LoadConst<ImmortalBool[True]>
    Branch<3>
  }

  bb 3 (preds 1, 2) {
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<4> v0 v1 v2 v5
    }
    v4 = Assign v2
    v2 = Assign v0
    v3 = Assign v1
    Return v5
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, ListExtend) {
  uint8_t bc[] = {LOAD_FAST, 0, LOAD_FAST, 1, LIST_EXTEND, 1, RETURN_VALUE, 0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

#if PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = ListExtend v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v0
    }
    Return v0
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<2> v0 v1
      }
    }
    v1 = CheckVar<"param1"> v1 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    v2 = ListExtend v0 v1 {
      FrameState {
        CurInstrOffset 4
        Locals<2> v0 v1
        Stack<1> v0
      }
    }
    Snapshot {
      CurInstrOffset 6
      Locals<2> v0 v1
      Stack<1> v0
    }
    Return v0
  }
}
)";
#endif
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, ListToTuple) {
  uint8_t bc[] = {
      LOAD_FAST,
      0,
#if PY_VERSION_HEX < 0x030C0000
      LIST_TO_TUPLE,
      0,
#else
      CALL_INTRINSIC_1,
      INTRINSIC_LIST_TO_TUPLE,
#endif
      RETURN_VALUE,
      0};
  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None});

#if PY_VERSION_HEX >= 0x030E0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<INTRINSIC_LIST_TO_TUPLE> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#elif PY_VERSION_HEX >= 0x030C0000
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v1 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v2 = CallIntrinsic<6> v0
    Snapshot {
      CurInstrOffset 4
      Locals<1> v0
      Stack<1> v2
    }
    Return v2
  }
}
)";
#else
  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    Snapshot {
      CurInstrOffset 0
      Locals<1> v0
    }
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 0
        Locals<1> v0
      }
    }
    v1 = MakeTupleFromList v0 {
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
  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
}

TEST_F(HIRBuildTest, LoadFastAndClear) {
#if PY_VERSION_HEX >= 0x030C0000
  uint8_t bc[] = {
      LOAD_FAST_AND_CLEAR, 1, LOAD_FAST_CHECK, 0, POP_TOP, 0, RETURN_VALUE, 0};

  std::unique_ptr<Function> irfunc = build_test(bc, {Py_None, Py_None});

  const char* expected = R"(fun jittestmodule:funcname {
  bb 0 {
    v0 = LoadArg<0; "param0">
    v2 = LoadCurrentFunc
    Snapshot {
      CurInstrOffset 0
      Locals<2> v0 v1
    }
    v3 = Assign v1
    v1 = LoadConst<Nullptr>
    v0 = CheckVar<"param0"> v0 {
      FrameState {
        CurInstrOffset 2
        Locals<2> v0 v1
        Stack<1> v3
      }
    }
    Return v3
  }
}
)";

  EXPECT_EQ(fullPrinter().ToString(*(irfunc)), expected);
#endif
}
