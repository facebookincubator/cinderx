// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/linear_scan.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_parser.h"
#include "cinderx/RuntimeTests/lir_query.h"

namespace cinderx::jit::lir {

class LIRPostGenerationRewriteTest : public RuntimeTest {};

static std::unique_ptr<Function> runPostGenRewrite(const char* lir_input_str) {
  auto func = Parser().parse(lir_input_str);
  codegen::Environ env;
  PostGenerationRewrite(func.get(), &env).run();
  return func;
}
static std::string runPostGenRewriteStr(const char* lir_input_str) {
  auto func = runPostGenRewrite(lir_input_str);
  return lirFuncString(*func);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsLoadSecondCallResultDataType) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* call = block->allocateInstr(
      Opcode::kCall,
      nullptr,
      OutVReg{DataType::kObject},
      Imm{0, DataType::k64bit});
  Instruction* second_result = block->allocateInstr(
      Opcode::kLoadSecondCallResult,
      nullptr,
      OutVReg{DataType::k16bit},
      VReg{call});
  Instruction* ret =
      block->allocateInstr(Opcode::kReturn, nullptr, VReg{second_result});

  codegen::Environ env;
  PostGenerationRewrite(&function, &env).run();

  ASSERT_EQ(block->getNumInstrs(), 3);
  EXPECT_EQ(second_result->opcode(), Opcode::kMove);
  EXPECT_EQ(second_result->output()->dataType(), DataType::k16bit);
  EXPECT_EQ(
      second_result->getInput(0)->getPhyRegister(),
      PhyLocation(codegen::arch::reg_general_auxilary_return_loc.loc, 16));
  EXPECT_EQ(ret->getInput(0)->getLinkedInstr(), second_result);
}

#if defined(CINDER_X86_64) && defined(_WIN32)
TEST_F(
    LIRPostGenerationRewriteTest,
    RewritesLoadSecondCallResultFromWindowsStructReturn) {
  std::string lir_input_str = fmt::format(
      R"(Function:
BB %0
  %10:Object = Move 4096
  %11:Object = Load [%10:Object]:Object
  {}:Object = Load [%10:Object + 0x8]:Object
  %12:Object = LoadSecondCallResult %11:Object
  Return %12:Object
)",
      codegen::arch::reg_general_auxilary_return_loc);

  auto func = runPostGenRewrite(lir_input_str.c_str());

  EXPECT_LIR_SEQUENCE(
      *func,
      Query(*func).opcode(Opcode::kLoad).with([](const Instruction* instr) {
        return instr->output()->isReg() &&
            instr->output()->getPhyRegister() ==
            codegen::arch::reg_general_auxilary_return_loc;
      }),
      Query(*func)
          .opcode(Opcode::kMove)
          .outVreg(12)
          .with([](const Instruction* instr) {
            return instr->getNumInputs() == 1 && instr->getInput(0)->isReg() &&
                instr->getInput(0)->getPhyRegister() ==
                codegen::arch::reg_general_auxilary_return_loc;
          }));
  EXPECT_NO_LIR(Query(*func).opcode(Opcode::kLoadSecondCallResult));
}
#endif

TEST_F(LIRPostGenerationRewriteTest, DoesNotAllowMultipleLSCRPerCall) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();

  Instruction* call =
      entry->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{0});
  Instruction* first_result = entry->allocateInstr(
      Opcode::kLoadSecondCallResult, nullptr, OutVReg{}, VReg{call});
  entry->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{first_result},
      Lbl{true_block},
      Lbl{false_block});

  Instruction* second_result = true_block->allocateInstr(
      Opcode::kLoadSecondCallResult, nullptr, OutVReg{}, VReg{call});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{second_result});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{call});

  codegen::Environ env;
  EXPECT_DEATH(
      PostGenerationRewrite(&function, &env).run(),
      "Call output consumed by multiple LoadSecondCallResult instructions");
}

TEST_F(LIRPostGenerationRewriteTest, MovesLoadSecondCallResultIntoPhiBlock) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* left = function.allocateBasicBlock();
  BasicBlock* right = function.allocateBasicBlock();
  BasicBlock* phi_block = function.allocateBasicBlock();
  BasicBlock* return_block = function.allocateBasicBlock();

  entry->addSuccessor(left);
  entry->addSuccessor(right);

  IncomingEdge left_edge = left->addSuccessor(phi_block);
  IncomingEdge right_edge = right->addSuccessor(phi_block);
  phi_block->addSuccessor(return_block);

  Instruction* entry_call =
      entry->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{0});
  entry->allocateInstr(
      Opcode::kCondBranch, nullptr, VReg{entry_call}, Lbl{left}, Lbl{right});

  Instruction* left_call =
      left->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{0});
  left->allocateInstr(Opcode::kBranch, nullptr, Lbl{phi_block});

  Instruction* right_call =
      right->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{0});
  right->allocateInstr(Opcode::kBranch, nullptr, Lbl{phi_block});

  Instruction* source_phi =
      phi_block->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  source_phi->addPhiInput(left_edge, left_call);
  source_phi->addPhiInput(right_edge, right_call);
  phi_block->allocateInstr(Opcode::kBranch, nullptr, Lbl{return_block});

  Instruction* rewritten = return_block->allocateInstr(
      Opcode::kLoadSecondCallResult,
      nullptr,
      OutVReg{DataType::k32bit},
      VReg{source_phi});
  return_block->allocateInstr(Opcode::kReturn, nullptr, VReg{rewritten});

  codegen::Environ env;
  PostGenerationRewrite(&function, &env).run();

  EXPECT_TRUE(rewritten->isPhi());
  EXPECT_EQ(rewritten->basicBlock(), source_phi->basicBlock());
  EXPECT_EQ(rewritten->output()->dataType(), DataType::k32bit);
  EXPECT_EQ(rewritten->numPhiInputs(), 2);
  EXPECT_NE(rewritten->phiInput(0), nullptr);
  EXPECT_NE(rewritten->phiInput(1), nullptr);
}

TEST_F(
    LIRPostGenerationRewriteTest,
    MovesLoadSecondCallResultThroughDuplicateEdges) {
  Function function;
  BasicBlock* source = function.allocateBasicBlock();
  BasicBlock* phi_block = function.allocateBasicBlock();

  IncomingEdge first_edge = source->addSuccessor(phi_block);
  IncomingEdge second_edge = source->addSuccessor(phi_block);

  Instruction* call =
      source->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{0});
  source->allocateInstr(
      Opcode::kCondBranch, nullptr, VReg{call}, Lbl{phi_block}, Lbl{phi_block});

  Instruction* phi = phi_block->allocateInstr(Opcode::kPhi, nullptr, OutVReg{});
  phi->addPhiInput(first_edge, call);
  phi->addPhiInput(second_edge, call);

  Instruction* rewritten = phi_block->allocateInstr(
      Opcode::kLoadSecondCallResult,
      nullptr,
      OutVReg{DataType::k32bit},
      VReg{phi});
  phi_block->allocateInstr(Opcode::kReturn, nullptr, VReg{rewritten});

  codegen::Environ env;
  PostGenerationRewrite(&function, &env).run();

  EXPECT_TRUE(rewritten->isPhi());
  EXPECT_EQ(rewritten->output()->dataType(), DataType::k32bit);
  EXPECT_EQ(rewritten->numPhiInputs(), 2);
  EXPECT_EQ(rewritten->phiPredecessor(0), rewritten->phiPredecessor(1));
}

TEST_F(LIRPostGenerationRewriteTest, RewritesLoadSecondCallResultThroughPhis) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %10 = Call 0
  CondBranch %10, BB%1, BB%2
BB %1 - succs: %3 %4
  %11 = Call 0
  CondBranch %11, BB%3, BB%4
BB %2 - succs: %20 %21
  %12 = Call 0
  CondBranch %12, BB%20, BB%21
BB %20 - succs: %22
  %120 = Call 0
  Branch BB%22
BB %21 - succs: %22
  %121 = Call 0
  Branch BB%22
BB %22 - succs: %5
  %122 = Phi (BB%20, %120), (BB%21, %121)
  Branch BB%5
BB %3 - succs: %5
  Call 0
  Branch BB%5
BB %4 - succs: %5
  Call 0
  Branch BB%5
BB %5 - succs: %6
  %13 = Phi (BB%22, %122), (BB%3, %11), (BB%4, %11), (BB%6, %13)
  %14:32bit = LoadSecondCallResult %13
  Branch BB%6
BB %6 - succs: %5
  Call 0
  Branch BB%5
)";

  std::string expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
      %10:Object = Call 0(0x0):64bit
                   CondBranch %10:Object, BB%1, BB%2

BB %1 - preds: %0 - succs: %3 %4
      %11:Object = Call 0(0x0):64bit
      %136:32bit = Move {0}:32bit
                   CondBranch %11:Object, BB%3, BB%4

BB %2 - preds: %0 - succs: %20 %21
      %12:Object = Call 0(0x0):64bit
                   CondBranch %12:Object, BB%20, BB%21

BB %20 - preds: %2 - succs: %22
     %120:Object = Call 0(0x0):64bit
      %138:32bit = Move {0}:32bit
                   Branch BB%22

BB %21 - preds: %2 - succs: %22
     %121:Object = Call 0(0x0):64bit
      %139:32bit = Move {0}:32bit
                   Branch BB%22

BB %22 - preds: %20 %21 - succs: %5
     %122:Object = Phi (BB%20, %120:Object), (BB%21, %121:Object)
      %137:32bit = Phi (BB%20, %138:32bit), (BB%21, %139:32bit)
                   Branch BB%5

BB %3 - preds: %1 - succs: %5
                   Call 0(0x0):64bit
                   Branch BB%5

BB %4 - preds: %1 - succs: %5
                   Call 0(0x0):64bit
                   Branch BB%5

BB %5 - preds: %3 %4 %6 %22 - succs: %6
      %13:Object = Phi (BB%3, %11:Object), (BB%4, %11:Object), (BB%6, %13:Object), (BB%22, %122:Object)
       %14:32bit = Phi (BB%3, %136:32bit), (BB%4, %136:32bit), (BB%6, %14:32bit), (BB%22, %137:32bit)
                   Branch BB%6

BB %6 - preds: %5 - succs: %5
                   Call 0(0x0):64bit
                   Branch BB%5

)",
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32});

  EXPECT_EQ(runPostGenRewriteStr(lir_input_str), expected_lir_str.c_str());
}

TEST_F(LIRPostGenerationRewriteTest, StrippedCallOperandsKeepLocalDefs) {
#if !defined(Py_GIL_DISABLED) || defined(CINDER_AARCH64)
  // TODO: Make the post-allocation assertions independent of x86-64 register
  // assignments so this can run on free-threaded AArch64 builds.
  SKIP("Object pointer stripping is only enabled in free-threadd builds");
#else
  Function func;
  BasicBlock* block = func.allocateBasicBlock();
  Instruction* first_call =
      block->allocateInstr(Opcode::kCall, nullptr, OutVReg{}, Imm{1});
  Instruction* first_arg =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{4369});
  Instruction* second_arg =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{8738});
  Instruction* third_arg =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{13107});
  Instruction* second_call = block->allocateInstr(
      Opcode::kCall, nullptr, OutVReg{}, Imm{3}, VReg{first_arg});
  Instruction* immediate_arg =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{4661});
  Instruction* final_arg =
      block->allocateInstr(Opcode::kMove, nullptr, OutVReg{}, Imm{17477});
  Instruction* vector_call = block->allocateInstr(
      Opcode::kVectorCallTstate,
      nullptr,
      OutVReg{},
      Imm{2},
      Imm{0},
      VReg{second_arg},
      VReg{first_call},
      VReg{first_arg},
      VReg{third_arg},
      VReg{second_call},
      VReg{immediate_arg},
      VReg{final_arg});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{vector_call});

  codegen::Environ env;
  PostGenerationRewrite(&func, &env).run();

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

  // A local copy of the long-lived call result is made before stripping.
  EXPECT_LIR(Query(func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::kObject)
                 .inVreg(0, first_call->id()));
  // ...and the strip produces the untagged value.
  EXPECT_LIR(
      Query(func).opcode(Opcode::kAnd).outType(DataType::kObjectUntagged));

  // Immediate PyObject* constants also flow through call-operand stripping.
  // They do not need a register-producing strip: the tag can be removed while
  // materializing the immediate as an ObjectUntagged value.
  EXPECT_LIR(Query(func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::kObjectUntagged)
                 .inImm(0, 4660));

  // The fragile shape is an `And` that directly consumes an older, non-local
  // tagged object definition. The `And` must not strip directly from a
  // long-lived definition: neither the call result nor the immediate argument
  // is stripped directly (each is copied locally first).
  EXPECT_NO_LIR(Query(func).opcode(Opcode::kAnd).inVreg(0, first_call->id()));
  EXPECT_NO_LIR(
      Query(func).opcode(Opcode::kAnd).inVreg(0, immediate_arg->id()));

  LinearScanAllocator allocator{&func};
  allocator.run();

  // Post-allocation the strip reads the register holding the adjacent copy.
  EXPECT_LIR(Query(func)
                 .opcode(Opcode::kMove)
                 .outPhyReg(codegen::RCX)
                 .outType(DataType::kObject)
                 .inPhyReg(0, codegen::RBX)
                 .inType(0, DataType::kObject));
  EXPECT_LIR(Query(func)
                 .opcode(Opcode::kAnd)
                 .outPhyReg(codegen::RBX)
                 .outType(DataType::kObjectUntagged)
                 .inPhyReg(0, codegen::RCX)
                 .inType(0, DataType::kObject));
  // Which register the immediate lands in is the allocator's business and
  // varies with the calling convention, so match everything but that.
  EXPECT_LIR(Query(func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::kObjectUntagged)
                 .inImm(0, 4660)
                 .inType(0, DataType::k64bit));
#endif
}

#if defined(CINDER_AARCH64)
TEST_F(LIRPostGenerationRewriteTest, LoadAbsoluteAddressUsesObjectDataType) {
#ifdef Py_GIL_DISABLED
  SKIP("Doesn't have expected LIR for deferred-refcount tag stripping yet");
#else
  const char* lir_input_str = R"(Function:
BB %0
  %10:Object = Load [0x12345]
  Return %10
)";

  const char* expected_lir_str = R"(Function:
BB %0
      %12:Object = Move 74565(0x12345):Object
      %10:Object = Load [%12:Object]:Object
                   Return %10:Object

)";

  EXPECT_EQ(runPostGenRewriteStr(lir_input_str), expected_lir_str);
#endif
}
#endif

} // namespace cinderx::jit::lir
