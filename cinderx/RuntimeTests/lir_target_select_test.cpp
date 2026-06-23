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

#include <memory>
#include <regex>
#include <sstream>
#include <string>

namespace jit::lir {

class LIRTargetSelectTest : public RuntimeTest {
 public:
  std::string getSelectedLIRString(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals) ||
        !PyDict_CheckExact(func->func_builtins)) {
      return "";
    }

    std::unique_ptr<hir::Function> irfunc(buildHIR(func));
    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

    codegen::Environ env;
    env.ctx = getContext();

    CodeRuntime runtime{func};
    runtime.setReifier(irfunc->reifier);
    env.code_rt = &runtime;

    LIRGenerator lir_gen(irfunc.get(), &env);
    std::unique_ptr<Function> lir_func = lir_gen.TranslateFunction();
    selectTargetOpcodes(lir_func.get());

    std::stringstream ss;
    lir_func->sortBasicBlocks();
    ss << *lir_func << '\n';
    return ss.str();
  }
};

#if defined(CINDER_AARCH64) || defined(CINDER_X86_64)
static std::string runTargetSelect(const char* lir_input_str) {
  std::unique_ptr<Function> func = Parser().parse(lir_input_str);
  selectTargetOpcodes(func.get());
  return fmt::format("{}", *func);
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(
      lir_str.find(":Object = Move 4294967296(0x100000000):64bit"),
      std::string::npos)
      << lir_str;
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
         %3:8bit = Equal %1:64bit, %2:64bit
        %4:64bit = Add %1:64bit, %2:64bit
                   CondBranch %3:8bit, BB%1, BB%2

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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("LessThanUnsigned"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Guard "), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("LessThanUnsigned"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Guard "), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("31(0x1f)"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Test32"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchS"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("BranchBitNotSet"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("31(0x1f)"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Test32"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchNS"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Test32"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Test32"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("BranchS"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Test32"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("BranchS"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("BranchS"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
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

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("BranchBitSet"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("31(0x1f)"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Test32"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("BranchS"), std::string::npos) << lir_str;
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

  std::string lir_str = getSelectedLIRString(pyfunc.get());

  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("BranchE"), std::string::npos);
  EXPECT_EQ(lir_str.find(" = Equal "), std::string::npos) << lir_str;
}
#endif

} // namespace jit::lir
