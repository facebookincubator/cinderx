// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/lir_parser.h"
#include "cinderx/RuntimeTests/lir_query.h"

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(CINDER_AARCH64)
#include <asmjit/arm/armutils.h>
#endif

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
    runtime->setReifier(Ref<>::create(irfunc->env.reifier));

    codegen::Environ env;
    env.reifier = irfunc->env.reifier;
    env.ctx = getContext();
    env.code_rt = runtime.get();

    LIRGenerator lir_gen(irfunc.get(), &env);
    std::unique_ptr<Function> lir_func = lir_gen.translateFunction();
    // Mirror the pipeline in gen_asm.cpp: DCE runs between generation and
    // target selection.
    eliminateDeadCode(lir_func.get());
    selectTargetOpcodes(lir_func.get());

    lir_func->sortBasicBlocks();
    return lir_func;
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

#if defined(CINDER_X86_64)
TEST_F(LIRTargetSelectTest, SelectsRegInputForLargeConstantStore) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* base = block->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutVReg{DataType::kObject},
      Imm{1, DataType::k64bit});
  block->allocateInstr(
      Opcode::kStore,
      nullptr,
      OutInd{base, 0x8, DataType::kObject},
      Imm{4294967296ULL, DataType::k64bit});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{base});

  selectTargetOpcodes(&function);

  // Should materialize large constant into a register.
  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::kObject)
                 .inImm(0, 4294967296ULL));
  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kStore)
                 .outInd(base->id(), 0x8)
                 .inDefOpcode(0, Opcode::kMove)
                 .inDefImm(0, 0, 4294967296ULL));
  EXPECT_NO_LIR(Query(function)
                    .opcode(Opcode::kStore)
                    .outInd(base->id(), 0x8)
                    .inImm(0, 4294967296ULL));
}
#endif

#if defined(CINDER_AARCH64)
// Mirrors fitsInSingleMovA64 in target_select.cpp for test preconditions.
bool testFitsInSingleMov(uint64_t value, unsigned bits) {
  if (bits == 32) {
    uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF0000u) == 0 || (v & 0xFFFF0000u) == 0xFFFF0000u ||
        (v & 0x0000FFFFu) == 0 || (v & 0x0000FFFFu) == 0x0000FFFFu) {
      return true;
    }
    return asmjit::arm::Utils::isLogicalImm(v, 32);
  }
  if (value <= 0xFFFFFFFFu) {
    uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF0000u) == 0 || (v & 0xFFFF0000u) == 0xFFFF0000u ||
        (v & 0x0000FFFFu) == 0 || (v & 0x0000FFFFu) == 0x0000FFFFu) {
      return true;
    }
  } else {
    int non_zero = 0;
    int all_ones = 0;
    for (int i = 0; i < 4; i++) {
      uint16_t hw = (value >> (i * 16)) & 0xFFFF;
      if (hw != 0) {
        non_zero++;
      }
      if (hw == 0xFFFF) {
        all_ones++;
      }
    }
    int movn_count = (all_ones == 4) ? 1 : (4 - all_ones);
    int best = non_zero < movn_count ? non_zero : movn_count;
    if (best == 1) {
      return true;
    }
  }
  return asmjit::arm::Utils::isLogicalImm(value, 64);
}

TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplier) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* base = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* index = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* lea =
      block->allocateInstr(Opcode::kLea, nullptr, OutVReg{DataType::k64bit});
  lea->allocateMemoryIndirectInput(base, index, 4, 0x8);
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{lea});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 16));
  EXPECT_LIR(Query(function).opcode(Opcode::kMulAdd).inVreg(0, index->id()));
  EXPECT_LIR(Query(function).opcode(Opcode::kAdd));
  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outVreg(lea->id())
                 .outType(DataType::k64bit));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kLea));
}

TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplierWithLargeOffset) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* base = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* index = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* lea =
      block->allocateInstr(Opcode::kLea, nullptr, OutVReg{DataType::k64bit});
  lea->allocateMemoryIndirectInput(base, index, 4, 0x100001);
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{lea});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 16));
  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 1048577));
  EXPECT_LIR(Query(function).opcode(Opcode::kMulAdd).inVreg(0, index->id()));
  EXPECT_LIR(Query(function).opcode(Opcode::kAdd));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kLea));
}

TEST_F(LIRTargetSelectTest, LegalizesComparisonOutputToMin32Bit) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* left = block->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutVReg{DataType::k64bit},
      Imm{1, DataType::k64bit});
  Instruction* right = block->allocateInstr(
      Opcode::kMove,
      nullptr,
      OutVReg{DataType::k64bit},
      Imm{2, DataType::k64bit});
  Instruction* comparison = block->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kEqual,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{comparison});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kCompare)
                 .condition(Condition::kEqual)
                 .outVreg(comparison->id())
                 .outType(DataType::k32bit));
  EXPECT_NO_LIR(Query(function)
                    .opcode(Opcode::kCompare)
                    .condition(Condition::kEqual)
                    .outVreg(comparison->id())
                    .outType(DataType::k8bit));
}

TEST_F(LIRTargetSelectTest, LegalizesBitwiseOutputToMin32Bit) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* left = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k8bit}, Imm{1});
  Instruction* right = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k8bit}, Imm{2});
  Instruction* bitwise = block->allocateInstr(
      Opcode::kAnd, nullptr, OutVReg{DataType::k8bit}, VReg{left}, VReg{right});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{bitwise});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kAnd)
                 .outVreg(bitwise->id())
                 .outType(DataType::k32bit));
  EXPECT_NO_LIR(Query(function)
                    .opcode(Opcode::kAnd)
                    .outVreg(bitwise->id())
                    .outType(DataType::k8bit));
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCForSingleUseCompare) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);
  Instruction* left = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* right = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* comparison = entry->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kEqual,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  entry->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{comparison},
      Lbl{true_block},
      Lbl{false_block});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{left});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{right});

  selectTargetOpcodes(&function);

  EXPECT_LIR_SEQUENCE(
      function,
      Query(function)
          .opcode(Opcode::kCmp)
          .inVreg(0, left->id())
          .inVreg(1, right->id()),
      Query(function).opcode(Opcode::kBranchCC).condition(Condition::kEqual));
  EXPECT_NO_LIR(
      Query(function).opcode(Opcode::kCompare).condition(Condition::kEqual));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kCondBranch));
}

TEST_F(
    LIRTargetSelectTest,
    SelectsBranchCCWhenInterveningInstructionsPreserveFlags) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);
  Instruction* left = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* right = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* comparison = entry->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kEqual,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  Instruction* intervening = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{3});
  entry->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{comparison},
      Lbl{true_block},
      Lbl{false_block});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{left});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{right});

  selectTargetOpcodes(&function);

  EXPECT_LIR_SEQUENCE(
      function,
      Query(function)
          .opcode(Opcode::kCmp)
          .inVreg(0, left->id())
          .inVreg(1, right->id()),
      Query(function)
          .opcode(Opcode::kMove)
          .outVreg(intervening->id())
          .inImm(0, 3),
      Query(function).opcode(Opcode::kBranchCC).condition(Condition::kEqual));
  EXPECT_NO_LIR(
      Query(function).opcode(Opcode::kCompare).condition(Condition::kEqual));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kCondBranch));
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCAcrossFlagClobber) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);
  Instruction* left = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* right = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* comparison = entry->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kEqual,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  Instruction* add = entry->allocateInstr(
      Opcode::kAdd,
      nullptr,
      OutVReg{DataType::k64bit},
      VReg{left},
      VReg{right});
  entry->allocateInstr(
      Opcode::kCondBranch,
      nullptr,
      VReg{comparison},
      Lbl{true_block},
      Lbl{false_block});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{left});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{right});

  selectTargetOpcodes(&function);

  EXPECT_LIR_SEQUENCE(
      function,
      Query(function)
          .opcode(Opcode::kAdd)
          .outVreg(add->id())
          .inVreg(0, left->id())
          .inVreg(1, right->id()),
      Query(function)
          .opcode(Opcode::kCmp)
          .inVreg(0, left->id())
          .inVreg(1, right->id()),
      Query(function).opcode(Opcode::kBranchCC).condition(Condition::kEqual));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kCompare));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kCondBranch));
}

TEST_F(LIRTargetSelectTest, LegalizesSignedSubWordInputs) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* left = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k8bit}, Imm{255});
  Instruction* right = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k16bit}, Imm{1});
  Instruction* comparison = block->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kSignedLT,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{comparison});

  selectTargetOpcodes(&function);

  EXPECT_LIR_SEQUENCE(
      function,
      Query(function)
          .opcode(Opcode::kSext)
          .outType(DataType::k32bit)
          .inVreg(0, left->id()),
      Query(function)
          .opcode(Opcode::kSext)
          .outType(DataType::k32bit)
          .inVreg(0, right->id()),
      Query(function)
          .opcode(Opcode::kCompare)
          .condition(Condition::kSignedLT)
          .outType(DataType::k32bit)
          .inDefOpcode(0, Opcode::kSext)
          .inDefOpcode(1, Opcode::kSext));
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCForSingleUseCompareGuard) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* left = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* right = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* comparison = block->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kUnsignedLT,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  block->allocateInstr(
      Opcode::kGuard, nullptr, Imm{kNotZero}, Imm{0}, VReg{comparison}, Imm{0});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{left});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kCmp)
                 .inVreg(0, left->id())
                 .inVreg(1, right->id()));
  EXPECT_LIR(Query(function).opcode(Opcode::kA64GuardCC));
  EXPECT_NO_LIR(Query(function)
                    .opcode(Opcode::kCompare)
                    .condition(Condition::kUnsignedLT));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kGuard));
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCThroughFlagPreservingInstrs) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* left = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  Instruction* right = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  Instruction* comparison = block->allocateInstr(
      Opcode::kCompare,
      nullptr,
      Condition::kUnsignedLT,
      OutVReg{DataType::k8bit},
      VReg{left},
      VReg{right});
  block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{8});
  block->allocateInstr(
      Opcode::kGuard, nullptr, Imm{kNotZero}, Imm{0}, VReg{comparison}, Imm{0});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{left});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kCmp)
                 .inVreg(0, left->id())
                 .inVreg(1, right->id()));
  EXPECT_LIR(Query(function).opcode(Opcode::kA64GuardCC));
  EXPECT_NO_LIR(Query(function)
                    .opcode(Opcode::kCompare)
                    .condition(Condition::kUnsignedLT));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kGuard));
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCAcrossFlagClobber) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  %4:64bit = Add %1, %2
  Guard 4, 0, %3, 0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR_SEQUENCE(
      *lir_func,
      Query(*lir_func).opcode(Opcode::kAdd).outVreg(4),
      Query(*lir_func).opcode(Opcode::kCmp).inVreg(0, 1).inVreg(1, 2),
      Query(*lir_func).opcode(Opcode::kA64GuardCC));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCompare));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kGuard));
}

TEST_F(LIRTargetSelectTest, SelectsA64SelectCCAcrossFlagClobber) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  %4:64bit = Add %1, %2
  %5:64bit = Select %3, %1, %2
  Return %5
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR_SEQUENCE(
      *lir_func,
      Query(*lir_func).opcode(Opcode::kAdd).outVreg(4),
      Query(*lir_func).opcode(Opcode::kCmp).inVreg(0, 1).inVreg(1, 2),
      Query(*lir_func)
          .opcode(Opcode::kA64SelectCC)
          .outVreg(5)
          .inImm(0, static_cast<uint64_t>(Condition::kUnsignedLT))
          .inVreg(1, 1)
          .inVreg(2, 2));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCompare));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kSelect));
}

TEST_F(LIRTargetSelectTest, LegalizesGuardFPInputToGPInput) {
  Function function;
  BasicBlock* block = function.allocateBasicBlock();
  Instruction* value = block->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::kDouble}, Imm{1});
  block->allocateInstr(
      Opcode::kGuard, nullptr, Imm{kNotZero}, Imm{0}, VReg{value}, Imm{0});
  block->allocateInstr(Opcode::kReturn, nullptr, VReg{value});

  selectTargetOpcodes(&function);

  // The FP input is moved into a GP register before the guard.
  EXPECT_LIR(Query(function)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inVreg(0, value->id())
                 .inType(0, DataType::kDouble));
  EXPECT_LIR(Query(function).guard(kNotZero, 0, DataType::k64bit));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kA64GuardCC));
}

TEST_F(LIRTargetSelectTest, SelectsBranchBitSetForTest32BranchS) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);
  Instruction* value = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  entry->allocateInstr(Opcode::kTest32, nullptr, VReg{value}, VReg{value});
  entry->allocateInstr(
      Opcode::kBranchCC, nullptr, Condition::kSign, Lbl{true_block});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{value});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{value});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function).opcode(Opcode::kBranchBitSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kTest32));
  EXPECT_NO_LIR(
      Query(function).opcode(Opcode::kBranchCC).condition(Condition::kSign));
}

TEST_F(LIRTargetSelectTest, SelectsBranchBitNotSetForTest32BranchNS) {
  Function function;
  BasicBlock* entry = function.allocateBasicBlock();
  BasicBlock* true_block = function.allocateBasicBlock();
  BasicBlock* false_block = function.allocateBasicBlock();
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);
  Instruction* value = entry->allocateInstr(
      Opcode::kMove, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  entry->allocateInstr(Opcode::kTest32, nullptr, VReg{value}, VReg{value});
  entry->allocateInstr(
      Opcode::kBranchCC, nullptr, Condition::kNotSign, Lbl{true_block});
  true_block->allocateInstr(Opcode::kReturn, nullptr, VReg{value});
  false_block->allocateInstr(Opcode::kReturn, nullptr, VReg{value});

  selectTargetOpcodes(&function);

  EXPECT_LIR(Query(function).opcode(Opcode::kBranchBitNotSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(function).opcode(Opcode::kTest32));
  EXPECT_NO_LIR(
      Query(function).opcode(Opcode::kBranchCC).condition(Condition::kNotSign));
}

TEST_F(LIRTargetSelectTest, SelectsCmpBranchForTestSupportedTypes) {
  for (const char* type : {"32bit", "64bit", "Object", "ObjectUntagged"}) {
    auto func = runTargetSelectFunc(
        fmt::format(
            R"(Function:
BB %0 - succs: %1 %2
  %1:{} = Move 1
  Test %1, %1
  BranchNZ BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)",
            type)
            .c_str());
    EXPECT_LIR(Query(*func).opcode(Opcode::kCmpBranchNonZero).inVreg(0, 1));
    EXPECT_NO_LIR(Query(*func).opcode(Opcode::kTest));
  }
}

TEST_F(LIRTargetSelectTest, SelectsCmpBranchForTest32OnlyWhen32Bit) {
  constexpr const char* pattern = R"(Function:
BB %0 - succs: %1 %2
  %1:{} = Move 1
  Test32 %1, %1
  BranchNZ BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto f32 = runTargetSelectFunc(fmt::format(pattern, "32bit").c_str());
  EXPECT_LIR(Query(*f32).opcode(Opcode::kCmpBranchNonZero).inVreg(0, 1));
  EXPECT_NO_LIR(Query(*f32).opcode(Opcode::kTest32));

  auto f64 = runTargetSelectFunc(fmt::format(pattern, "64bit").c_str());
  EXPECT_LIR(Query(*f64).opcode(Opcode::kTest32));
  EXPECT_NO_LIR(Query(*f64).opcode(Opcode::kCmpBranchNonZero));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectCmpBranchForNarrowTest) {
  for (const char* type : {"8bit", "16bit"}) {
    auto func = runTargetSelectFunc(
        fmt::format(
            R"(Function:
BB %0 - succs: %1 %2
  %1:{} = Move 1
  Test %1, %1
  BranchNZ BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)",
            type)
            .c_str());
    EXPECT_LIR(Query(*func).opcode(Opcode::kTest));
    EXPECT_NO_LIR(Query(*func).opcode(Opcode::kCmpBranchNonZero));
  }
}

TEST_F(LIRTargetSelectTest, SelectsCmpBranchForZeroAndNonZeroPolarities) {
  struct {
    const char* test;
    const char* branch;
    Opcode expected;
  } cases[] = {
      {"Test", "BranchZ", Opcode::kCmpBranchZero},
      {"Test", "BranchE", Opcode::kCmpBranchZero},
      {"Test", "BranchNZ", Opcode::kCmpBranchNonZero},
      {"Test", "BranchNE", Opcode::kCmpBranchNonZero},
      {"Test32", "BranchZ", Opcode::kCmpBranchZero},
      {"Test32", "BranchE", Opcode::kCmpBranchZero},
      {"Test32", "BranchNZ", Opcode::kCmpBranchNonZero},
      {"Test32", "BranchNE", Opcode::kCmpBranchNonZero},
  };

  for (const auto& [test, branch, expected] : cases) {
    auto func = runTargetSelectFunc(
        fmt::format(
            R"(Function:
BB %0 - succs: %1 %2
  %1:32bit = Move 1
  {} %1, %1
  {} BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)",
            test,
            branch)
            .c_str());
    EXPECT_LIR(Query(*func).opcode(expected).inVreg(0, 1));
    EXPECT_NO_LIR(Query(*func).opcode(
        test == std::string_view("Test") ? Opcode::kTest : Opcode::kTest32));
  }
}

TEST_F(LIRTargetSelectTest, DoesNotSelectCmpBranchWhenNotAdjacent) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 0
  %2:64bit = Move 1
  Test %1, %1
  %3:64bit = Move 2
  BranchNZ BB%1
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %1
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kTest));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCmpBranchNonZero));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchWhenFlagsReadLater) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 0
  %2:64bit = Move 1
  %3:64bit = Move 0
  Test %1, %1
  BranchNZ BB%1
  %4:64bit = A64SelectCC 0, %2, %3
BB %1 - preds: %0
  Return %2
BB %2 - preds: %0
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kTest));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCmpBranchNonZero));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchWhenFlagsReadAfterArithmetic) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 0
  %2:64bit = Move 1
  %3:64bit = Move 0
  Test %1, %1
  BranchNZ BB%1
  %5:64bit = Mul %2, %3
  %4:64bit = A64SelectCC 0, %2, %3
BB %1 - preds: %0
  Return %2
BB %2 - preds: %0
  Return %3
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kTest));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCmpBranchNonZero));
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

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kBranchBitSet));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kTest32));
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

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kTest32));
  EXPECT_LIR(
      Query(*lir_func).opcode(Opcode::kBranchCC).condition(Condition::kSign));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kBranchBitSet));
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

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kTest32));
  EXPECT_LIR(
      Query(*lir_func).opcode(Opcode::kBranchCC).condition(Condition::kSign));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kBranchBitSet));
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

  EXPECT_LIR(
      Query(*lir_func).opcode(Opcode::kBranchCC).condition(Condition::kSign));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kBranchBitSet));
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

  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kBranchBitSet).inImm(1, 31));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kTest32));
  EXPECT_NO_LIR(
      Query(*lir_func).opcode(Opcode::kBranchCC).condition(Condition::kSign));
}

TEST_F(LIRTargetSelectTest, SelectsBitTestForPythonCompareBranch) {
  const char* src = R"(
def func(x, y):
  if x in y:
    return x
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getSelectedLIRFunction(pyfunc.get());

  // The `in` result is a known bool, so the branch tests a bit that
  // differs between the Py_True/Py_False addresses instead of comparing
  // against the True singleton (testing another differing bit, or xoring
  // the tested bit, when a nonzero test of the first differing bit would
  // mean "condition false"). The bypassed compare and its True load are dead
  // and eliminated.
  auto true_addr = reinterpret_cast<uintptr_t>(Py_True);
  auto false_addr = reinterpret_cast<uintptr_t>(Py_False);
  auto mask = uintptr_t{1} << std::countr_zero(true_addr ^ false_addr);
  bool bit_in_true = (true_addr & mask) != 0;
  // `if x in y` compares `== True`, which holds for True, so the layout is
  // inverted exactly when the mask bit is set in Py_False.
  bool inverted = !bit_in_true;
  // Mirror the generator's alternate-bit search for the inverted layout.
  uintptr_t alt_mask = 0;
  if (inverted) {
    uintptr_t candidates = (true_addr ^ false_addr) & true_addr;
    if (candidates != 0) {
      alt_mask = uintptr_t{1} << std::countr_zero(candidates);
    }
  }
  Opcode test_op = (inverted && alt_mask == 0) ? Opcode::kXor : Opcode::kAnd;
  uintptr_t test_mask = alt_mask != 0 ? alt_mask : mask;
  EXPECT_LIR(Query(*lir_func).opcode(Opcode::kAnd).inImm(1, test_mask));
  if (test_op == Opcode::kXor) {
    EXPECT_LIR(Query(*lir_func).opcode(Opcode::kXor).inImm(1, mask));
  }
  EXPECT_LIR(
      Query(*lir_func).opcode(Opcode::kCondBranch).inDefOpcode(0, test_op));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCompare));
  EXPECT_NO_LIR(Query(*lir_func).inAddr(0, true_addr));
}

TEST_F(LIRTargetSelectTest, SelectsBitTestForBoolSingletonComparisons) {
  // Branches on a known bool compared against a True/False singleton use a
  // bit-test of a differing address bit, testing another differing bit (or
  // xoring the tested bit) when a nonzero test of the first differing bit
  // would mean "condition false"; the bypassed compare dies in DCE. (The
  // `==`/`!=` inputs still load the singleton for the outer comparison, so
  // only the test itself is asserted here.)
  struct Case {
    const char* cond;
    // Whether the HIR compare holds for True (i.e. `== True`, `!= False`).
    bool compare_true_for_true;
  };
  Case cases[] = {
      {"(x in y) == True", true},
      {"(x in y) != True", false},
      {"(x in y) == False", false},
      {"(x in y) != False", true},
      {"(x in y) is True", true},
      {"(x in y) is False", false},
      {"(x in y) is not True", false},
      {"not (x in y)", true},
      {"bool(x)", true},
  };
  auto true_addr = reinterpret_cast<uintptr_t>(Py_True);
  auto false_addr = reinterpret_cast<uintptr_t>(Py_False);
  auto mask = uintptr_t{1} << std::countr_zero(true_addr ^ false_addr);
  bool bit_in_true = (true_addr & mask) != 0;
  for (auto [cond, compare_true_for_true] : cases) {
    std::string src = "def func(x, y):\n  if " + std::string(cond) +
        ":\n    return 1\n  return 2\n";
    Ref<PyObject> pyfunc(compileAndGet(src.c_str(), "func"));
    ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling " << cond;
    auto lir_func = getSelectedLIRFunction(pyfunc.get());
    bool inverted = (bit_in_true != compare_true_for_true);
    // Mirror the generator's alternate-bit search for the inverted layout.
    uintptr_t alt_mask = 0;
    if (inverted) {
      uintptr_t satisfying = compare_true_for_true ? true_addr : false_addr;
      uintptr_t candidates = (true_addr ^ false_addr) & satisfying;
      if (candidates != 0) {
        alt_mask = uintptr_t{1} << std::countr_zero(candidates);
      }
    }
    Opcode test_op = (inverted && alt_mask == 0) ? Opcode::kXor : Opcode::kAnd;
    uintptr_t test_mask = alt_mask != 0 ? alt_mask : mask;
    EXPECT_LIR(Query(*lir_func).opcode(Opcode::kAnd).inImm(1, test_mask))
        << "peephole did not fire for " << cond;
    if (test_op == Opcode::kXor) {
      EXPECT_LIR(Query(*lir_func).opcode(Opcode::kXor).inImm(1, mask))
          << "peephole did not fire for " << cond;
    }
    EXPECT_LIR(
        Query(*lir_func).opcode(Opcode::kCondBranch).inDefOpcode(0, test_op))
        << "branch is not on the bit-test for " << cond;
    EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCompare))
        << "bypassed compare survived for " << cond;
  }
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCAcrossAddFromStaticPython) {
  const char* src = R"(
from __static__ import int64

def func(x: int64, y: int64) -> int64:
  cond = x < y
  value = x + y
  if cond:
    return value
  return x
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getSelectedLIRFunction(pyfunc.get());

  EXPECT_LIR_SEQUENCE(
      *lir_func,
      Query(*lir_func).opcode(Opcode::kAdd),
      Query(*lir_func).opcode(Opcode::kCmp),
      Query(*lir_func)
          .opcode(Opcode::kBranchCC)
          .condition(Condition::kSignedLT));
}

TEST_F(LIRTargetSelectTest, SelectsA64SelectCCForStaticPythonCompareResult) {
  const char* src = R"(
from __static__ import box, int64

def func(x: int64, y: int64) -> bool:
  return box(x < y)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getSelectedLIRFunction(pyfunc.get());

  EXPECT_LIR_SEQUENCE(
      *lir_func,
      Query(*lir_func).opcode(Opcode::kCmp),
      Query(*lir_func).opcode(Opcode::kA64SelectCC));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kCompare));
  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kSelect));
}

TEST_F(LIRTargetSelectTest, SelectsConditionalXorForImmortalBoolBox) {
  // The transform below only applies when this build's immortal Py_True and
  // Py_False addresses differ by a logical-immediate-encodable constant and
  // both need more than a single mov.
  auto addr = [](PyObject* o) { return reinterpret_cast<uint64_t>(o); };
  uint64_t true_addr = addr(Py_True);
  uint64_t false_addr = addr(Py_False);
  uint64_t diff = true_addr ^ false_addr;
  if (!asmjit::arm::Utils::isLogicalImm(diff, 64)) {
    SKIP("Py_True/Py_False diff not a logical immediate here");
  }
  if (testFitsInSingleMov(true_addr, 64) ||
      testFitsInSingleMov(false_addr, 64)) {
    SKIP("Py_True/Py_False fit in a single mov here");
  }

  const char* src = R"(
from __static__ import box, int64

def func(x: int64, y: int64) -> bool:
  return box(x < y)
)";

  Ref<PyObject> pyfunc(compileStaticAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  auto lir_func = getSelectedLIRFunction(pyfunc.get());

  // The boxed comparison becomes a conditional xor-flip of one materialized
  // address: keep = Move(Py_True|Py_False),
  // flipped = Xor(keep, Py_True ^ Py_False), A64SelectCC(lt, flipped, keep).
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kA64SelectCC)
                 .with([&](const Instruction* select) {
                   if (select->getNumInputs() < 2) {
                     return false;
                   }
                   const Operand* in = select->getInput(1);
                   if (in == nullptr || !in->isLinked()) {
                     return false;
                   }
                   const Instruction* flip = in->getLinkedInstr();
                   return flip->getNumInputs() == 2 &&
                       flip->opcode() == Opcode::kXor &&
                       flip->getInput(1)->isImm() &&
                       flip->getInput(1)->getConstant() == diff;
                 }));

  // Exactly one of the two immortal addresses is materialized now.
  int num_materialized = 0;
  for (const BasicBlock* bb : lir_func->basicBlocks()) {
    for (const std::unique_ptr<Instruction>& instr : bb->instructions()) {
      if (instr->opcode() != Opcode::kMove || instr->getNumInputs() < 1) {
        continue;
      }
      const Operand* in = instr->getInput(0);
      if (in->isImm() &&
          (in->getConstant() == true_addr || in->getConstant() == false_addr)) {
        ++num_materialized;
      }
    }
  }
  EXPECT_EQ(num_materialized, 1) << lirFuncString(*lir_func);
}

TEST_F(LIRTargetSelectTest, SelectsXorForLargeConstantPair) {
  constexpr uint64_t kV1 = 1311768467463790320ULL; // 0x123456789ABCDEF0
  constexpr uint64_t kV2 = 1311768467463790224ULL; // 0x123456789ABCDE90
  constexpr uint64_t kDiff = 0x60;
  ASSERT_EQ(kV1 ^ kV2, kDiff);
  ASSERT_FALSE(testFitsInSingleMov(kV1, 64));
  ASSERT_FALSE(testFitsInSingleMov(kV2, 64));
  ASSERT_TRUE(asmjit::arm::Utils::isLogicalImm(kDiff, 64));

  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:64bit = Select %1, 1311768467463790320, 1311768467463790224
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kSelect)
                 .with([&](const Instruction* select) {
                   const Operand* in = select->getInput(1);
                   if (in == nullptr || !in->isLinked()) {
                     return false;
                   }
                   const Instruction* flip = in->getLinkedInstr();
                   return flip->opcode() == Opcode::kXor &&
                       flip->getInput(1)->isImm() &&
                       flip->getInput(1)->getConstant() == kDiff;
                 }));
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Opcode::kMove)
                    .outType(DataType::k64bit)
                    .inImm(0, kV1));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, kV2));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectXorForMoveConstants) {
  // The xor transform only applies to two direct immediates; constants
  // already materialized by Moves are left alone.
  constexpr uint64_t kV1 = 1311768467463790320ULL; // 0x123456789ABCDEF0
  constexpr uint64_t kV2 = 1311768467463790224ULL; // 0x123456789ABCDE90
  ASSERT_FALSE(testFitsInSingleMov(kV1, 64));
  ASSERT_FALSE(testFitsInSingleMov(kV2, 64));
  ASSERT_TRUE(asmjit::arm::Utils::isLogicalImm(kV1 ^ kV2, 64));

  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1311768467463790320
  %2:64bit = Move 1311768467463790224
  %3:8bit = Move 1
  %4:64bit = Select %3, %1, %2
  Return %4
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kXor));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, kV1));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, kV2));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectXorWhenConstantsFitSingleMov) {
  ASSERT_TRUE(testFitsInSingleMov(1, 64));
  ASSERT_TRUE(testFitsInSingleMov(2, 64));
  ASSERT_TRUE(asmjit::arm::Utils::isLogicalImm(1 ^ 2, 64));

  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:64bit = Select %1, 1, 2
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kXor));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 1));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, 2));
}

TEST_F(LIRTargetSelectTest, DoesNotSelectXorWhenDiffNotLogicalImm) {
  constexpr uint64_t kV1 = 1311768467463790320ULL; // 0x123456789ABCDEF0
  constexpr uint64_t kV2 = 1311768467463855857ULL; // 0x123456789ABDDEF1
  constexpr uint64_t kDiff = 0x10001;
  ASSERT_EQ(kV1 ^ kV2, kDiff);
  ASSERT_FALSE(testFitsInSingleMov(kV1, 64));
  ASSERT_FALSE(testFitsInSingleMov(kV2, 64));
  ASSERT_FALSE(asmjit::arm::Utils::isLogicalImm(kDiff, 64));

  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:64bit = Select %1, 1311768467463790320, 1311768467463855857
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_NO_LIR(Query(*lir_func).opcode(Opcode::kXor));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, kV1));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k64bit)
                 .inImm(0, kV2));
}

TEST_F(LIRTargetSelectTest, SelectsXorFor32BitLargeConstants) {
  constexpr uint64_t kV1 = 305419896ULL; // 0x12345678
  constexpr uint64_t kV2 = 305419800ULL; // 0x12345618
  constexpr uint64_t kDiff = 0x60;
  ASSERT_EQ(kV1 ^ kV2, kDiff);
  ASSERT_FALSE(testFitsInSingleMov(kV1, 32));
  ASSERT_FALSE(testFitsInSingleMov(kV2, 32));
  ASSERT_TRUE(asmjit::arm::Utils::isLogicalImm(kDiff, 32));

  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:32bit = Select %1, 305419896:32bit, 305419800:32bit
  Return %2
)";

  auto lir_func = runTargetSelectFunc(lir_input_str);

  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kXor)
                 .outType(DataType::k32bit)
                 .inImm(1, kDiff));
  EXPECT_NO_LIR(Query(*lir_func)
                    .opcode(Opcode::kMove)
                    .outType(DataType::k32bit)
                    .inImm(0, kV1));
  EXPECT_LIR(Query(*lir_func)
                 .opcode(Opcode::kMove)
                 .outType(DataType::k32bit)
                 .inImm(0, kV2));
}
#endif

} // namespace cinderx::jit::lir
