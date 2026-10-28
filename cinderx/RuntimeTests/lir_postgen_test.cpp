// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/linear_scan.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_query.h"

namespace cinderx::jit::lir {

class LIRPostGenerationRewriteTest : public RuntimeTest {};

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
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* load = block->allocateInstr(
      Opcode::kLoad,
      nullptr,
      OutVReg{DataType::kObject},
      MemImm{reinterpret_cast<void*>(0x12345), DataType::kObject});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{load});

  codegen::Environ env;
  PostGenerationRewrite(&function, &env).run();

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::kObject)
                 .inImm(0, 0x12345)
                 .inType(0, DataType::kObject));
  EXPECT_TRUE(load->getInput(0)->isInd());
#endif
}

TEST_F(LIRPostGenerationRewriteTest, StoreImmediateZeroToIndirectNotLowered) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* base = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{0});
  Instruction* store_zero = block->allocateInstr(
      Opcode::kStore,
      nullptr,
      OutInd{base, 72, DataType::k16bit},
      Imm{0, DataType::k16bit});
  store_zero->output()->setDataType(DataType::k16bit);
  Instruction* store_nonzero = block->allocateInstr(
      Opcode::kStore,
      nullptr,
      OutInd{base, 74, DataType::k16bit},
      Imm{42, DataType::k16bit});
  store_nonzero->output()->setDataType(DataType::k16bit);

  codegen::Environ env;
  PostGenerationRewrite(&function, &env).run();

  // store_zero should keep its immediate 0 operand unchanged
  EXPECT_TRUE(store_zero->getInput(0)->isImm());
  EXPECT_EQ(store_zero->getInput(0)->getConstant(), 0);

  // store_nonzero should have been lowered to a Move to vreg
  EXPECT_TRUE(store_nonzero->getInput(0)->isLinked());
  EXPECT_EQ(
      store_nonzero->getInput(0)->getLinkedInstr()->opcode(), Opcode::kMove);
}
#endif

} // namespace cinderx::jit::lir
