// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_query.h"

#include <math.h>

#include <memory>
#include <ostream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using namespace asmjit;
using namespace cinderx::jit;
using namespace cinderx::jit::lir;

TEST(LIRTypeTest, DataTypeByteShift) {
  EXPECT_EQ(byteShift(DataType::k8bit), 0);
  EXPECT_EQ(byteShift(DataType::k16bit), 1);
  EXPECT_EQ(byteShift(DataType::k32bit), 2);
  EXPECT_EQ(byteShift(DataType::k64bit), 3);
  EXPECT_EQ(byteShift(DataType::kDouble), 3);
  EXPECT_EQ(byteShift(DataType::kObject), 3);
}

class LIRGeneratorTest : public RuntimeTest {
 public:
  std::unique_ptr<Function> getLIRFunction(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals)) {
      return nullptr;
    }

    if (!PyDict_CheckExact(func->func_builtins)) {
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
    runtime->setReifier(irfunc->reifier);

    codegen::Environ env;
    env.ctx = getContext();
    env.code_rt = runtime.get();

    LIRGenerator lir_gen(irfunc.get(), &env);

    auto lir_func = lir_gen.translateFunction();

    lir_func->sortBasicBlocks();
    return lir_func;
  }

  std::string getLIRString(PyObject* func_obj) {
    auto lir_func = getLIRFunction(func_obj);
    if (!lir_func) {
      return "";
    }
    std::stringstream ss;
    ss << *lir_func << '\n';
    return ss.str();
  }

  std::string removeCommentsAndWhitespace(const std::string& input_s) {
    std::istringstream iss(input_s);
    std::string line;
    std::string output_s;
    while (std::getline(iss, line)) {
      if (line.length() == 0) {
        // skip blank lines
        continue;
      } else if (line.length() > 0 && line.at(0) == '#') {
        // skip comments
        continue;
      } else {
        output_s += line + '\n';
      }
    }
    return output_s;
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

TEST_F(LIRGeneratorTest, StaticLoadInteger) {
  const char* pycode = R"(
from __static__ import int64

def f() -> int64:
  d: int64 = 12
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 12));
}

TEST_F(LIRGeneratorTest, StaticLoadDouble) {
  const char* pycode = R"(
from __static__ import double

def f() -> double:
  d: double = 3.1415
  return d
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // Check that the resulting LIR has the unboxed constant we care about,
  // without hardcoding a variable name or the program structure.
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 4614256447914709615ULL));
}

TEST_F(LIRGeneratorTest, StaticBoxDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 3.1415
  return box(d)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Instruction::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 4614256447914709615ULL));
  EXPECT_LIR(
      Query(*lir_func).opcode(Instruction::kCall).outType(DataType::kObject));
}

TEST_F(LIRGeneratorTest, StaticAddDouble) {
  const char* pycode = R"(
from __static__ import double, box

def f() -> float:
  d: double = 1.14
  e: double = 2.00
  return box(d + e)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(pycode, "f"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  // `d + e` on doubles should lower to an Fadd.
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kFadd));
}

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_Fallthrough) {
  const char* src = R"(
def func2(x):
  y = 0
  if x:
    y = 100
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Load %5, 8(0x8)
             %10 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %10, %9, %8
                   CondBranch %10, BB%14, BB%13

BB %13 - preds: %7

BB %14 - preds: %7
             %15 = Load %5, 16(0x10)

BB %16 - preds: %13 %14
             %17 = Phi (BB%14, %15), (BB%13, %9)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

// disabled due to unstable Guard instruction
TEST_F(LIRGeneratorTest, DISABLED_CondBranch) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());

  auto lir_expected = fmt::format(
      R"(Function:
BB %0
              %1 = Bind RDI
              %2 = Bind RSI
              %3 = Bind RDX
              %4 = Bind R9
              %5 = Bind R10
              %6 = Bind R11

BB %7 - preds: %0
              %8 = Load %2, 0(0x0)
              %9 = Call {0}({0:#x}), %8
                   Guard 1(0x1), 0(0x0), %9, %8
                   CondBranch %9, BB%16, BB%12

BB %12 - preds: %7
             %13 = Load %5, 16(0x10)
                   Call {1}({1:#x}), %13
                   Return %13

BB %16 - preds: %7
             %17 = Load %5, 8(0x8)
                   Call {1}({1:#x}), %17
                   Return %17

BB %20 - preds: %12 %16
             RDI = Move %6


)",
      reinterpret_cast<uint64_t>(PyObject_IsTrue),
      reinterpret_cast<uint64_t>(Py_IncRef));
  ASSERT_EQ(lir_str, lir_expected);
}

TEST_F(LIRGeneratorTest, ParserDataTypeTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %7 %10
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10
       %8:Object = Move [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserMemIndTest) {
  auto lir_str = fmt::format(
      R"(Function:
BB %0
        %1:64bit = Bind {}:Object
        %2:64bit = Move [{}:Object + {}:Object * 8 + 0x8]:Object
        %3:64bit = Move [%2:64bit + 0x3]:Object
        %4:64bit = Move [%2:64bit + %3:64bit * 16]:Object
[%4:64bit - 0x16]:Object = Move [{}:Object + %4:64bit]:Object

)",
      PhyLocation{5, 64},
      PhyLocation{5, 64},
      PhyLocation{4, 64},
      PhyLocation{0, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  // Assume that the parser assigns basic block and register numbers
  // based on the parsing order of the instructions.
  // If the parser behavior is modified and assigns numbers differently,
  // then the assert may fail.
  ASSERT_EQ(lir_str, ss.str());
}

TEST_F(LIRGeneratorTest, ParserTest) {
  const char* pycode = R"(
def func(x):
    if x:
        return True
    return False
)";

  Ref<PyObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = removeCommentsAndWhitespace(getLIRString(pyfunc.get()));

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  std::stringstream ss;
  parsed_func->sortBasicBlocks();
  ss << *parsed_func;
  ASSERT_EQ(lir_str, removeCommentsAndWhitespace(ss.str()));
}

TEST_F(LIRGeneratorTest, ParserSectionTest) {
  std::string lir_str = fmt::format(
      R"(Function:
BB %0 - section: hot
         %1:8bit = Bind {}:8bit
        %2:32bit = Bind {}:32bit
        %3:16bit = Bind {}:16bit
        %4:64bit = Bind {}:64bit
       %5:Object = Move 0(0x0):Object
                   CondBranch %5:Object, BB%7, BB%10

BB %7 - preds: %0 - succs: %10 - section: .coldtext
       %8:Object = Move [0x5]:Object
                   Return %8:Object

BB %10 - preds: %0 %7 - section: hot

)",
      PhyLocation{0, 8},
      PhyLocation{6, 32},
      PhyLocation{9, 16},
      PhyLocation{10, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  ASSERT_EQ(parsed_func->basicBlocks().size(), 3);
  ASSERT_EQ(
      parsed_func->basicBlocks()[0]->section(), codegen::CodeSection::kHot);
  ASSERT_EQ(
      parsed_func->basicBlocks()[1]->section(), codegen::CodeSection::kCold);
  ASSERT_EQ(
      parsed_func->basicBlocks()[2]->section(), codegen::CodeSection::kHot);
}

template <typename... Args>
std::string formatMemoryIndirect(Args&&... args) {
  MemoryIndirect im(nullptr);
  im.setMemoryIndirect(std::forward<Args>(args)...);
  return fmt::format("{}", im);
}

TEST(LIRTest, MemoryIndirectTests) {
  auto base = codegen::ARGUMENT_REGS[3];
  auto index = codegen::ARGUMENT_REGS[2];

  ASSERT_EQ(fmt::format("[{}:Object]", base), formatMemoryIndirect(base.loc));
  ASSERT_EQ(
      fmt::format("[{}:Object + 0x7fff]", base),
      formatMemoryIndirect(base.loc, 0x7fff));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 4]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 2));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object + 0x100]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 0, 0x100));
  ASSERT_EQ(
      fmt::format("[{}:Object + {}:Object * 2 + 0x1000]", base, index),
      formatMemoryIndirect(base.loc, index.loc, 1, 0x1000));
}

extern "C" uint64_t __Invoke_PyTuple_Check(PyObject* obj);

TEST_F(LIRGeneratorTest, CondBranchCheckTypeEmitsCallToSubclassCheck) {
  const char* hir = R"(
fun foo {
  bb 0 {
    v0 = LoadArg<0>
    CondBranchCheckType<1, 2, Tuple> v0
  }

  bb 1 {
    v0 = LoadConst<NoneType>
    Branch<2>
  }

  bb 2 {
    Return v0
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  codegen::Environ env;

  env.ctx = getContext();

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.translateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';

  std::string lir_str = ss.str();
  lir_str.erase(
      std::remove(lir_str.begin(), lir_str.end(), '\n'), lir_str.end());

  std::string lir_expected_re = fmt::format(
      R"(# CondBranchCheckType<1, 3, Tuple> v1\s+%\d+:8bit = Call {0}\({0:#x}\):64bit, %\d+:Object\s+CondBranch %\d+:8bit, BB%\d+, BB%\d+)",
      reinterpret_cast<uint64_t>(__Invoke_PyTuple_Check));

  std::regex re(lir_expected_re);
  if (!std::regex_search(lir_str, re)) {
    FAIL() << "Couldn't find expected string \n"
           << lir_expected_re << '\n'
           << "In:\n"
           << lir_str << '\n';
  }
}

TEST_F(LIRGeneratorTest, UnreachableFollowsBottomType) {
  const char* hir_source = R"(fun test {
  bb 0 {
    v7 = LoadConst<Nullptr>
    v8 = CheckVar<"a"> v7 {
      FrameState {
        CurInstrOffset 2
        Locals<1> v7
      }
    }
    Unreachable
  }
}
)";

  std::unique_ptr<hir::Function> irfunc = hir::HIRParser{}.parseHIR(hir_source);
  ASSERT_NE(irfunc, nullptr);

  Compiler::runPasses(
      *irfunc,
      static_cast<PassConfig>(
          // We don't have a code-object for kInsertUpdatePrevInstr.
          PassConfig::kAllExceptInliner & ~PassConfig::kInsertUpdatePrevInstr));

  codegen::Environ env;
  CodeRuntime code_runtime{irfunc->code, irfunc->builtins, irfunc->globals};

  env.ctx = getContext();
  env.code_rt = &code_runtime;

  LIRGenerator lir_gen(irfunc.get(), &env);

  auto lir_func = lir_gen.translateFunction();

  std::stringstream ss;

  lir_func->sortBasicBlocks();
  ss << *lir_func << '\n';
#if PY_VERSION_HEX >= 0x030E0000
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
      %11:Object = Phi
                   EpilogueEnd %11:Object


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation{7, 64}
#else
      PhyLocation{0, 64}
#endif
  );
#else
  auto lir_expected = fmt::format(
      R"(Function:
BB %0 - succs: %1

BB %1 - preds: %0 - succs: %6
                   SetupFrame
       %3:Object = Bind {}:Object
       %4:Object = Bind {}:Object
       %5:Object = Bind {}:Object

BB %6 - preds: %1

# v9:Nullptr = LoadConst<Nullptr>
       %7:Object = Move 0(0x0):Object

# v10:Bottom = CheckVar<"a"> v9 {{
#   LiveValues<1> unc:v9
#   FrameState {{
#     CurInstrOffset 2
#     Locals<1> v9
#   }}
# }}
                   Guard 4(0x4):64bit, 0(0x0):64bit, %7:Object, 0(0x0):64bit, %7:Object

# Unreachable
                   Unreachable

BB %10
      %11:Object = Phi
                   EpilogueEnd %11:Object


)",
      PhyLocation{10, 64},
      PhyLocation{11, 64},
#if defined(CINDER_X86_64)
      PhyLocation{7, 64}
#else
      PhyLocation{0, 64}
#endif
  );
#endif
  ASSERT_EQ(ss.str(), lir_expected.c_str());
}

TEST_F(LIRGeneratorTest, UnstableGlobals) {
  getMutableConfig().stable_frame = false;

  const char* src = R"(
def func1(x):
  return x + 1

def func2(x):
  return func1(x) + 2

def func3(x):
  def inner(x2):
    return func1(x2) + 4
  return inner(3)
)";

  Ref<PyObject> pyfunc2(compileAndGet(src, "func2"));
  ASSERT_NE(pyfunc2.get(), nullptr) << "Failed compiling func";

  auto lir_func2 = getLIRFunction(pyfunc2.get());

  auto fast_addr = reinterpret_cast<uint64_t>(rt::loadGlobal);
  auto slow_addr = reinterpret_cast<uint64_t>(rt::loadGlobalFromThreadState);

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_NO_LIR(
      Query(*lir_func2).opcode(Instruction::kCall).inAddr(0, fast_addr))
      << "Should not call out to rt::loadGlobal as globals aren't stable";
  EXPECT_LIR(Query(*lir_func2).opcode(Instruction::kCall).inAddr(0, slow_addr))
      << "Should be calling out to rt::loadGlobalFromThreadState as globals "
         "aren't stable";

  Ref<PyObject> pyfunc3(compileAndGet(src, "func3"));
  ASSERT_NE(pyfunc3.get(), nullptr) << "Failed compiling func";

  auto lir_func3 = getLIRFunction(pyfunc3.get());

  auto slow_addr2 = reinterpret_cast<uint64_t>(rt::loadGlobalsDict);

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_LIR(Query(*lir_func3).opcode(Instruction::kCall).inAddr(0, slow_addr2))
      << "Should be calling out to rt::loadGlobalsDict as globals "
         "aren't stable";
}

TEST_F(LIRGeneratorTest, AttrCachesOff) {
  getMutableConfig().attr_caches = false;

  const char* src = R"(
import sys

def func():
  return sys.argv
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  auto fast_addr = reinterpret_cast<uint64_t>(LoadAttrCache::invoke);
  auto slow_addr = reinterpret_cast<uint64_t>(PyObject_GetAttr);

  EXPECT_FALSE(getConfig().attr_caches);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kCall).inAddr(0, slow_addr))
      << "Should be calling out to PyObject_GetAttr as inline caches are "
         "disabled";
  EXPECT_NO_LIR(
      Query(*lir_func).opcode(Instruction::kCall).inAddr(0, fast_addr))
      << "Should not be calling out to LoadAttrCache::invoke as inline caches "
         "are disabled";
}

TEST_F(LIRGeneratorTest, UnstableCode) {
  getMutableConfig().stable_frame = false;

  const char* src = R"(
import sys

def func():
  return sys.argv
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());

  auto slow_addr = reinterpret_cast<uint64_t>(rt::loadName);

  EXPECT_FALSE(getConfig().stable_frame);

  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kCall).inAddr(0, slow_addr))
      << "Should be calling out to rt::loadName as code objects aren't "
         "stable";
}

TEST_F(LIRGeneratorTest, LoadEvalBreakerUsesMoveRelaxed) {
  // Backward jumps (loop back-edges) emit LoadEvalBreaker in HIR to check
  // whether the interpreter needs to handle pending events. This should lower
  // to MoveRelaxed in LIR.
  const char* src = R"(
def func():
  x = 0
  while x < 10:
    x += 1
  return x
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getLIRFunction(pyfunc.get());
  EXPECT_LIR(Query(*lir_func).opcode(Instruction::kMoveRelaxed))
      << "LoadEvalBreaker should lower to MoveRelaxed";
}

TEST_F(LIRGeneratorTest, ListDynamicIndexLoadStoreUsesScaledArrayLIR) {
  const char* src = R"(
import os

def func(value):
  xs = [1, 2, 3]
  i = 1 if os.argv else 2
  old = xs[i]
  xs[i] = value
  return old
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_str = getLIRString(pyfunc.get());
  const std::regex scaled_array_load{
      R"(:Object = Move \[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object)"};
  const std::regex scaled_array_store{
      R"(\[%\d+:\w+ \+ %\d+:\w+ \* 8(?: \+ 0x0)?\]:Object = Move %\d+:Object)"};

  EXPECT_TRUE(std::regex_search(lir_str, scaled_array_load)) << lir_str;
  EXPECT_TRUE(std::regex_search(lir_str, scaled_array_store)) << lir_str;
}
