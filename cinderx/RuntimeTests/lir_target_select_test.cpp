// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_query.h"

#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace cinderx::jit::lir {

class LIRTargetSelectTest : public RuntimeTest {
 public:
  std::unique_ptr<Function> getSelectedLIRFunction(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals) ||
        !PyDict_CheckExact(func->func_builtins)) {
      return nullptr;
    }

    // The returned LIR keeps references into the HIR function and CodeRuntime
    // (e.g. the printer emits the originating HIR instruction as a comment), so
    // they must outlive it. Retain them for the lifetime of the fixture.
    std::unique_ptr<hir::Function>& irfunc =
        hir_funcs_.emplace_back(buildHIR(func));
    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

    std::unique_ptr<CodeRuntime>& runtime =
        runtimes_.emplace_back(std::make_unique<CodeRuntime>(func));
    runtime->setReifier(ThreadedRef<>::create(irfunc->env.reifier));

    codegen::Environ env;
    env.reifier = irfunc->env.reifier;
    env.ctx = getContext();
    env.code_rt = runtime.get();

    LIRGenerator lir_gen(irfunc.get(), &env);
    std::unique_ptr<Function> lir_func = lir_gen.translateFunction();
    selectTargetOpcodes(lir_func.get());

    lir_func->sortBasicBlocks();
    return lir_func;
  }

  std::string getSelectedLIRString(PyObject* func_obj) {
    auto lir_func = getSelectedLIRFunction(func_obj);
    if (!lir_func) {
      return "";
    }
    std::stringstream ss;
    ss << *lir_func << '\n';
    return ss.str();
  }

  void TearDown() override {
    // These hold Python references and must be destroyed before the base
    // fixture finalizes the interpreter.
    runtimes_.clear();
    hir_funcs_.clear();
    RuntimeTest::TearDown();
  }

 private:
  // Keep the HIR functions and runtimes referenced by LIR produced in this
  // fixture alive until the test finishes.
  std::vector<std::unique_ptr<hir::Function>> hir_funcs_;
  std::vector<std::unique_ptr<CodeRuntime>> runtimes_;
};

#if defined(CINDER_AARCH64) || defined(CINDER_X86_64)
static std::unique_ptr<Function> runTargetSelectFunc(
    const char* lir_input_str) {
  std::unique_ptr<Function> func = Parser().parse(lir_input_str);
  selectTargetOpcodes(func.get());
  return func;
}
#endif

// Only used by the AARCH64-only golden-output tests below.
#ifdef CINDER_AARCH64
static std::string runTargetSelect(const char* lir_input_str) {
  auto func = runTargetSelectFunc(lir_input_str);
  return lirFuncString(*func);
}
#endif

#if defined(CINDER_X86_64)
TEST_F(LIRTargetSelectTest, SelectsRegInputForLargeConstantMemoryMove) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:Object = Move 1
  [%1:Object + 0x8]:Object = Move 4294967296
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  // Should materialize large constant into a register.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::kObject)
                 .inImm(0, 4294967296ULL));
  // The memory-indirect store shape isn't modelled by Query yet, so check
  // it (and the absence of a direct immediate store) via the formatted text.
  std::string lir_str = lirFuncString(*lir_func);
  EXPECT_TRUE(
      std::regex_search(
          lir_str,
          std::regex{R"(\[%1:Object \+ 0x8\]:Object = Move %\d+:Object)"}))
      << lir_str;
  EXPECT_EQ(
      lir_str.find("[%1:Object + 0x8]:Object = Move 4294967296(0x100000000)"),
      std::string::npos)
      << lir_str;
}
#endif

#if defined(CINDER_AARCH64)
TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplier) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:64bit = Lea [%1:64bit + %2:64bit * 16 + 0x8]
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 16));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kMulAdd).inVreg(0, 2));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kAdd));
  // The Lea result %3 is materialized by a Move.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outVreg(3)
                 .outType(DataType::k64bit));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kLea));
}

TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplierWithLargeOffset) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:64bit = Lea [%1:64bit + %2:64bit * 16 + 0x100001]
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 16));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 1048577));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kMulAdd).inVreg(0, 2));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kAdd));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kLea));
}

TEST_F(LIRTargetSelectTest, LegalizesComparisonOutputToMin32Bit) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kEqual)
                 .outVreg(3)
                 .outType(DataType::k32bit));
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Instruction::kEqual)
                    .outVreg(3)
                    .outType(DataType::k8bit));
}

TEST_F(LIRTargetSelectTest, LegalizesBitwiseOutputToMin32Bit) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:8bit = Move 2
  %3:8bit = And %1, %2
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kAnd)
                 .outVreg(3)
                 .outType(DataType::k32bit));
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Instruction::kAnd)
                    .outVreg(3)
                    .outType(DataType::k8bit));
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCForSingleUseCompare) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
                   Cmp %1:64bit, %2:64bit
                   BranchE

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(
    LIRTargetSelectTest,
    SelectsBranchCCWhenInterveningInstructionsPreserveFlags) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  %4:64bit = Move 3
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
                   Cmp %1:64bit, %2:64bit
        %4:64bit = Move 3(0x3):64bit
                   BranchE

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchCCAcrossFlagClobber) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  %4:64bit = Add %1, %2
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
        %3:32bit = Equal %1:64bit, %2:64bit
        %4:64bit = Add %1:64bit, %2:64bit
                   CondBranch %3:32bit, BB%1, BB%2

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCForSingleUseCompareGuard) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  Guard 4, 0, %3, 0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kCmp));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kA64GuardCC));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kLessThanUnsigned));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kGuard));
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCThroughFlagPreservingInstrs) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  %4:64bit = Move 8
  Guard 4, 0, %3, 0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kCmp));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kA64GuardCC));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kLessThanUnsigned));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kGuard));
}

TEST_F(LIRTargetSelectTest, LegalizesGuardFPInputToGPInput) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:Double = Move 1
  Guard 4, 0, %1, 0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  // The FP input %1 is moved into a GP register before the guard.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inVreg(0, 1)
                 .inType(0, DataType::kDouble));
  // The Guard's operand shapes aren't modelled by Query yet; check by text.
  std::string lir_str = lirFuncString(*lir_func);
  EXPECT_TRUE(
      std::regex_search(
          lir_str,
          std::regex{R"(Guard 4\(0x4\):64bit, 0\(0x0\):64bit, %\d+:64bit)"}))
      << lir_str;
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kA64GuardCC));
}

TEST_F(LIRTargetSelectTest, SelectsBranchBitSetForTest32BranchS) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  Test32 %1, %1
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kTest32));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchS));
}

TEST_F(LIRTargetSelectTest, SelectsBranchBitNotSetForTest32BranchNS) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  Test32 %1, %1
  BranchNS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(
      Query(*lir_func).opcode(Instruction::kBranchBitNotSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kTest32));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchNS));
}

TEST_F(
    LIRTargetSelectTest,
    SelectsBranchBitSetWhenInterveningInstructionsPreserveFlags) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  Test32 %1, %1
  %2:64bit = Move 2
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kTest32));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchBitSetAcrossFlagClobber) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  Test32 %1, %1
  %2:64bit = Add %1, %1
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kTest32));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchS));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchBitSetFromEarlierTest32) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  Test32 %1, %1
  Test32 %1, %2
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kTest32));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchS));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchBitSetWithoutFlagProducer) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchS));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet));
}

TEST_F(LIRTargetSelectTest, SelectsBranchBitSetForPythonRefcountSignTest) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:Object = Move 1
  %2:32bit = Move [%1:Object]:Object
  Test32 %2, %2
  BranchS BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchBitSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kTest32));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kBranchS));
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCForPythonCompareBranch) {
  const char* src = R"(
def func(x, y):
  if x in y:
    return x
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getSelectedLIRFunction(pyfunc.get());

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kCmp));
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kBranchE));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Instruction::kEqual));
}
#endif

} // namespace cinderx::jit::lir
