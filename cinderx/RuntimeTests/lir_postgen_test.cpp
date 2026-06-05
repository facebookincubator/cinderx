// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace jit::lir {
class LIRPostGenerationRewriteTest : public RuntimeTest {};

static std::string runPostGenRewrite(const char* lir_input_str) {
  auto func = Parser().parse(lir_input_str);
  codegen::Environ env;
  PostGenerationRewrite(func.get(), &env).run();
  return fmt::format("{}", *func);
}

static std::string runRegAlloc(const char* lir_input_str) {
  auto func = Parser().parse(lir_input_str);
  LinearScanAllocator allocator{func.get()};
  allocator.run();
  return fmt::format("{}", *func);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsLoadSecondCallResultDataType) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  %11:16bit = LoadSecondCallResult %10
  Return %11
)";

  std::string expected_lir_str = fmt::format(
      R"(Function:
BB %0
{}      %10:Object = Call {}
       %11:16bit = Move {}:16bit
                   Return %11:16bit

)",
      "",
      "0(0x0):64bit",
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 16});

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str.c_str());
}

TEST_F(LIRPostGenerationRewriteTest, DoesNotAllowMultipleLSCRPerCall) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  %11 = LoadSecondCallResult %10
  CondBranch %11, BB%1, BB%2
BB %1
  %12 = LoadSecondCallResult %10
  Return %12
BB %2
  Return %10
)";

  EXPECT_DEATH(
      runPostGenRewrite(lir_input_str),
      "Call output consumed by multiple LoadSecondCallResult instructions");
}

TEST_F(LIRPostGenerationRewriteTest, RewritesLoadSecondCallResultThroughPhis) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  CondBranch %10, BB%1, BB%2
BB %1
  %11 = Call 0
  CondBranch %11, BB%3, BB%4
BB %2
  %12 = Call 0
  CondBranch %12, BB%20, BB%21
BB %20
  %120 = Call 0
  Branch BB%22
BB %21
  %121 = Call 0
  Branch BB%22
BB %22
  %122 = Phi BB%20, %120, BB%21, %121
  Branch BB%5
BB %3
  Call 0
  Branch BB%5
BB %4
  Call 0
  Branch BB%5
BB %5
  %13 = Phi BB%22, %122, BB%3, %11, BB%4, %11, BB%6, %13
  %14:32bit = LoadSecondCallResult %13
  Branch BB%6
BB %6
  Call 0
  Branch BB%5
)";

  std::string expected_lir_str = fmt::format(
      R"(Function:
BB %0
      %10:Object = Call 0(0x0):64bit
                   CondBranch %10:Object, BB%1, BB%2

BB %1
      %11:Object = Call 0(0x0):64bit
      %139:32bit = Move {0}:32bit
                   CondBranch %11:Object, BB%3, BB%4

BB %2
      %12:Object = Call 0(0x0):64bit
                   CondBranch %12:Object, BB%20, BB%21

BB %20
     %120:Object = Call 0(0x0):64bit
      %137:32bit = Move {0}:32bit
                   Branch BB%22

BB %21
     %121:Object = Call 0(0x0):64bit
      %138:32bit = Move {0}:32bit
                   Branch BB%22

BB %22
     %122:Object = Phi (BB%20, %120:Object), (BB%21, %121:Object)
      %136:32bit = Phi (BB%20, %137:32bit), (BB%21, %138:32bit)
                   Branch BB%5

BB %3
                   Call 0(0x0):64bit
                   Branch BB%5

BB %4
                   Call 0(0x0):64bit
                   Branch BB%5

BB %5
      %13:Object = Phi (BB%22, %122:Object), (BB%3, %11:Object), (BB%4, %11:Object), (BB%6, %13:Object)
       %14:32bit = Phi (BB%22, %136:32bit), (BB%3, %139:32bit), (BB%4, %139:32bit), (BB%6, %14:32bit)
                   Branch BB%6

BB %6
                   Call 0(0x0):64bit
                   Branch BB%5

)",
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32});

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str.c_str());
}

TEST_F(LIRPostGenerationRewriteTest, StrippedCallOperandsKeepLocalDefs) {
#ifndef Py_GIL_DISABLED
  GTEST_SKIP() << "Object pointer stripping is only enabled in free-threaded "
                  "builds";
#else
  const char* lir_input_str = R"(Function:
BB %0
  %10:Object = Call 1
  %20:Object = Move 4369
  %30:Object = Move 8738
  %40:Object = Move 13107
  %50:Object = Call 3, %20
  %60:Object = Move 4661
  %70:Object = Move 17477
  %80:Object = VectorCallTstate 2, 0, %30, %10, %20, %40, %50, %60, %70
  Return %80
)";

  std::string pre_alloc_lir = runPostGenRewrite(lir_input_str);
  std::string allocated_lir = runRegAlloc(pre_alloc_lir.c_str());

  // This constructs the postgen/regalloc hazard directly.  VectorCallTstate
  // receives PyObject* operands that must have deferred-RC tag bits stripped
  // before entering C.  The original rewrite built each strip directly from
  // the operand's defining instruction:
  //
  //   %strip:ObjectUntagged = And %base:Object, ~tagbits
  //   %call:Object = VectorCallTstate ..., %strip, ...
  //
  // That is valid LIR before register allocation, but it is fragile when
  // `%base` is a long-lived tagged Python object.  Calls can force regalloc to
  // split/spill long-lived object intervals and reuse their physical registers
  // while preparing the call.  If the strip remains tied directly to `%base`,
  // regalloc can rewrite the strip to read a physical register after that
  // register has been reused for a different value.
  //
  // A representative bad post-allocation shape is:
  //
  //   RCX:Object = Move <original tagged object>
  //   ...
  //   [RBP(-2456)]:Object = Move RCX:Object
  //   RCX:Object = Move <different object>
  //   RAX:ObjectUntagged = And RCX:Object, ~tagbits
  //   [RBP(-2464)]:ObjectUntagged = Move RAX:ObjectUntagged
  //   RAX:Object = VectorCallTstate ..., [RBP(-2464)], ...
  //
  // The call receives an untagged value derived from the different object
  // instead of the original tagged object.  The fix inserts an adjacent copy
  // before stripping:
  //
  //   %copy:Object = Move %base:Object
  //   %strip:ObjectUntagged = And %copy:Object, ~tagbits
  //
  // This test checks both phases: pre-allocation LIR has the local
  // copy-then-strip shape, and post-allocation LIR strips from the register
  // assigned to that adjacent copy.
  EXPECT_NE(pre_alloc_lir.find(":Object = Move %10:Object"), std::string::npos);
  EXPECT_NE(pre_alloc_lir.find(":ObjectUntagged = And %"), std::string::npos);
  EXPECT_NE(
      allocated_lir.find("RCX:Object = Move RBX:Object"), std::string::npos);
  EXPECT_NE(
      allocated_lir.find("RBX:ObjectUntagged = And RCX:Object"),
      std::string::npos);

  // Immediate PyObject* constants also flow through call-operand stripping.
  // They do not need a register-producing strip: the tag can be removed while
  // materializing the immediate as an ObjectUntagged value.
  EXPECT_NE(
      pre_alloc_lir.find(":ObjectUntagged = Move 4660(0x1234):64bit"),
      std::string::npos);
  EXPECT_NE(
      allocated_lir.find("RAX:ObjectUntagged = Move 4660(0x1234):64bit"),
      std::string::npos);

  // The fragile shape is an `And` that directly consumes an older, non-local
  // tagged object definition.  The call-result operand and immediate-like
  // operand in this input used to keep that shape.
  EXPECT_EQ(pre_alloc_lir.find("= And %10:Object"), std::string::npos);
  EXPECT_EQ(pre_alloc_lir.find("= And %60:Object"), std::string::npos);
#endif
}

#if defined(CINDER_AARCH64)
TEST_F(LIRPostGenerationRewriteTest, MoveAbsoluteAddressUsesObjectDataType) {
  const char* lir_input_str = R"(Function:
BB %0
  %10:Object = Move [0x12345]
  Return %10
)";

  const char* expected_lir_str = R"(Function:
BB %0
      %12:Object = Move 74565(0x12345):Object
      %10:Object = Move [%12:Object]:Object
                   Return %10:Object

)";

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str);
}
#endif

} // namespace jit::lir
