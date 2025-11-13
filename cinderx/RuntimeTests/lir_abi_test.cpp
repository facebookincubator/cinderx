// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace asmjit;
using namespace jit;
using namespace jit::codegen;

namespace jit::lir {

// Test each combination of instructions and operands that are implicitly
// permitted by the translation layer.
class LIRABITest : public RuntimeTest {
 public:
  // Used when operands need a pointer to a function.
  static void testImmPtrTarget(void) {}

  // Create an instruction, initialize it with its opcode and operands, then run
  // it through autogen to lower it.
  template <typename... T>
  void translateInstrWithOrigin(
      Instruction::Opcode opcode,
      const hir::Instr* origin,
      T&&... args) {
    hir::Function hirFunction;

    Environ environ;
    environ.rt = Runtime::get();
    environ.code_rt = environ.rt->allocateCodeRuntime(
        hirFunction.code.get(),
        hirFunction.builtins.get(),
        hirFunction.globals.get());

    auto code_allocator =
        std::unique_ptr<ICodeAllocator>(CodeAllocator::make());

    CodeHolder code;
    code.init(code_allocator->asmJitEnvironment());

    arch::Builder as(&code);
    environ.as = &as;

    Function function;
    BasicBlock bb(&function);

    // Allocate the instruction and any additional resources that it may need.
    auto insn = bb.allocateInstr(opcode, origin, args...);
    switch (opcode) {
      case Instruction::kBranch:
      case Instruction::kBranchZ:
      case Instruction::kBranchNZ:
      case Instruction::kBranchA:
      case Instruction::kBranchB:
      case Instruction::kBranchAE:
      case Instruction::kBranchBE:
      case Instruction::kBranchG:
      case Instruction::kBranchL:
      case Instruction::kBranchGE:
      case Instruction::kBranchLE:
      case Instruction::kBranchC:
      case Instruction::kBranchNC:
      case Instruction::kBranchO:
      case Instruction::kBranchNO:
      case Instruction::kBranchS:
      case Instruction::kBranchNS:
      case Instruction::kBranchE:
      case Instruction::kBranchNE:
        environ.block_label_map.emplace(&bb, as.newLabel());
        insn->addOperands(Lbl{&bb});
        break;
      case Instruction::kDeoptPatchpoint:
      case Instruction::kGuard:
      case Instruction::kYieldFrom:
      case Instruction::kYieldFromHandleStopAsyncIteration:
      case Instruction::kYieldFromSkipInitialSend:
      case Instruction::kYieldValue:
        environ.code_rt->addDeoptMetadata(DeoptMetadata{});
        break;
      case Instruction::kYieldInitial:
        environ.code_rt->addDeoptMetadata(DeoptMetadata{});
        environ.initial_yield_spill_size_ = 16;
        break;
      default:
        break;
    }

    // Translate the instruction using the auto translator.
    autogen::AutoTranslator::getInstance().translateInstr(&environ, insn);
  }

  template <typename... T>
  void translateInstr(Instruction::Opcode opcode, T&&... args) {
    translateInstrWithOrigin(opcode, nullptr /* origin */, args...);
  }

  Imm makeImmPtr(void (*ptr)(void) = LIRABITest::testImmPtrTarget) {
    return Imm{reinterpret_cast<uintptr_t>(ptr)};
  }

  Ind makeInd(int loc = 0, int32_t offset = 0) {
    return Ind{PhyLocation(loc), offset};
  }

  Ind makeIndScale(
      int base = 0,
      int index = 0,
      uint8_t scale = 0,
      int32_t offset = 0) {
    return Ind{PhyLocation(base), PhyLocation(index), scale, offset};
  }

  OutInd makeOutInd(int loc = 0, int32_t offset = 0) {
    return OutInd{PhyLocation(loc), offset};
  }

  OutInd makeOutIndScale(
      int base = 0,
      int index = 0,
      uint8_t scale = 0,
      int32_t offset = 0) {
    return OutInd{PhyLocation(base), PhyLocation(index), scale, offset};
  }

  PhyReg makePhyReg(int loc = 0, DataType type = DataType::k64bit) {
    return PhyReg{PhyLocation(loc, bitSize(type)), type};
  }

  OutPhyReg makeOutPhyReg(int loc = 0, DataType type = DataType::k64bit) {
    return OutPhyReg{PhyLocation(loc, bitSize(type)), type};
  }

  PhyReg makePhyRegFP(int loc = VECD_REG_BASE) {
    return PhyReg{PhyLocation(loc, 64), DataType::kDouble};
  }

  OutPhyReg makeOutPhyRegFP(int loc = VECD_REG_BASE) {
    return OutPhyReg{PhyLocation(loc, 64), DataType::kDouble};
  }

  Stk makeStk(int loc = -16, DataType type = DataType::kObject) {
    return Stk{PhyLocation(loc, bitSize(type)), type};
  }

  OutStk makeOutStk(int loc = -16, DataType type = DataType::kObject) {
    return OutStk{PhyLocation(loc, bitSize(type)), type};
  }
};

// kLea R m
TEST_F(LIRABITest, TestkLea_OutPhyReg_Mem) {
  translateInstr(Instruction::kLea, makeOutPhyReg(), makeStk());
  translateInstr(Instruction::kLea, makeOutPhyReg(), MemImm{nullptr});
  translateInstr(Instruction::kLea, makeOutPhyReg(), makeInd(1, 16));
  translateInstr(Instruction::kLea, makeOutPhyReg(), makeIndScale(1, 2, 8, 16));
}

// kCall R i
TEST_F(LIRABITest, TestkCall_OutPhyReg_Imm) {
  translateInstr(Instruction::kCall, makeOutPhyReg(), makeImmPtr());
}

// kCall R r
TEST_F(LIRABITest, TestkCall_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kCall, makeOutPhyReg(), makePhyReg());
}

// kCall i
TEST_F(LIRABITest, TestkCall_Imm) {
  translateInstr(Instruction::kCall, makeImmPtr());
}

// kCall r
TEST_F(LIRABITest, TestkCall_PhyReg) {
  translateInstr(Instruction::kCall, makePhyReg());
}

// kCall m
TEST_F(LIRABITest, TestkCall_Stk) {
  translateInstr(Instruction::kCall, makeStk());
}

// kMove R r
TEST_F(LIRABITest, TestkMove_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kMove, makeOutPhyReg(), makePhyReg());
}

// kMove R i
TEST_F(LIRABITest, TestkMove_OutPhyReg_Imm) {
  translateInstr(Instruction::kMove, makeOutPhyReg(), Imm{0});
  translateInstr(Instruction::kMove, makeOutPhyReg(), Imm{UINT32_MAX});
  translateInstr(Instruction::kMove, makeOutPhyReg(), Imm{UINT32_MAX + 1});
  translateInstr(Instruction::kMove, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Instruction::kMove, makeOutPhyReg(), FPImm{0.0});
}

// kMove R m
TEST_F(LIRABITest, TestkMove_OutPhyReg_Mem) {
  translateInstr(Instruction::kMove, makeOutPhyReg(), makeStk());
  translateInstr(Instruction::kMove, makeOutPhyReg(), MemImm{nullptr});
  translateInstr(Instruction::kMove, makeOutPhyReg(), makeInd(1, 16));
  translateInstr(
      Instruction::kMove, makeOutPhyReg(), makeIndScale(1, 2, 8, 16));
}

// kMove R x
TEST_F(LIRABITest, TestkMove_OutPhyReg_FPPhyReg) {
  translateInstr(Instruction::kMove, makeOutPhyReg(), makePhyRegFP());
}

// kMove M r
TEST_F(LIRABITest, TestkMove_Mem_PhyReg) {
  translateInstr(Instruction::kMove, makeOutStk(), makePhyReg());
  translateInstr(Instruction::kMove, OutMemImm{nullptr}, makePhyReg());
  translateInstr(Instruction::kMove, makeOutInd(1, 16), makePhyReg());
  translateInstr(
      Instruction::kMove, makeOutIndScale(1, 2, 8, 16), makePhyReg());
}

// kMove M i
TEST_F(LIRABITest, TestkMove_Mem_Imm) {
  translateInstr(Instruction::kMove, makeOutStk(), Imm{0});
  translateInstr(Instruction::kMove, makeOutStk(), Imm{UINT64_MAX});
  translateInstr(Instruction::kMove, OutMemImm{nullptr}, Imm{0});
  translateInstr(Instruction::kMove, OutMemImm{nullptr}, Imm{UINT64_MAX});
  translateInstr(Instruction::kMove, makeOutInd(1, 16), Imm{0});
  translateInstr(Instruction::kMove, makeOutInd(1, 16), Imm{UINT64_MAX});
  translateInstr(Instruction::kMove, makeOutIndScale(1, 2, 8, 16), Imm{0});
  translateInstr(
      Instruction::kMove, makeOutIndScale(1, 2, 8, 16), Imm{UINT64_MAX});
  translateInstr(Instruction::kMove, makeOutStk(), FPImm{0.0});
  translateInstr(Instruction::kMove, OutMemImm{nullptr}, FPImm{0.0});
  translateInstr(Instruction::kMove, makeOutInd(1, 16), FPImm{0.0});
  translateInstr(Instruction::kMove, makeOutIndScale(1, 2, 8, 16), FPImm{0.0});
}

// kMove M x
TEST_F(LIRABITest, TestkMove_Mem_FPPhyReg) {
  translateInstr(Instruction::kMove, makeOutStk(), makePhyRegFP());
  translateInstr(Instruction::kMove, OutMemImm{nullptr}, makePhyRegFP());
  translateInstr(Instruction::kMove, makeOutInd(1, 16), makePhyRegFP());
  translateInstr(
      Instruction::kMove, makeOutIndScale(1, 2, 8, 16), makePhyRegFP());
}

// kMove X x
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kMove, makeOutPhyRegFP(), makePhyRegFP(VECD_REG_BASE + 1));
}

// kMove X m
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_Mem) {
  translateInstr(Instruction::kMove, makeOutPhyRegFP(), makeStk());
  translateInstr(Instruction::kMove, makeOutPhyRegFP(), MemImm{nullptr});
  translateInstr(Instruction::kMove, makeOutPhyRegFP(), makeInd(1, 16));
  translateInstr(
      Instruction::kMove, makeOutPhyRegFP(), makeIndScale(1, 2, 8, 16));
}

// kMove X r
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_PhyReg) {
  translateInstr(Instruction::kMove, makeOutPhyRegFP(), makePhyReg());
}

// kGuard ANY
TEST_F(LIRABITest, TestkGuard) {
  translateInstr(Instruction::kGuard, Imm{kAlwaysFail}, Imm{0}, Imm{0}, Imm{0});
  translateInstr(
      Instruction::kGuard, Imm{kHasType}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(
      Instruction::kGuard,
      Imm{kHasType},
      Imm{0},
      makePhyReg(),
      MemImm{nullptr});
  translateInstr(
      Instruction::kGuard, Imm{kHasType}, Imm{0}, makePhyReg(), makePhyReg());
  translateInstr(Instruction::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(
      Instruction::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), MemImm{nullptr});
  translateInstr(
      Instruction::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), makePhyReg());
  translateInstr(
      Instruction::kGuard, Imm{kNotNegative}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(
      Instruction::kGuard, Imm{kNotZero}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(Instruction::kGuard, Imm{kZero}, Imm{0}, makePhyReg(), Imm{0});
}

// kDeoptPatchpoint ANY
TEST_F(LIRABITest, TestkDeoptPatchpoint) {
  jit::CodePatcher patcher;

  uint16_t value = 0xFF00;
  std::array<uint8_t, 2> bytes{0xEF, 0xBE};
  patcher.link(reinterpret_cast<uintptr_t>(&value), bytes);

  translateInstr(Instruction::kDeoptPatchpoint, MemImm{&patcher}, Imm{0});
}

// kNegate r
TEST_F(LIRABITest, TestkNegate_PhyReg) {
  translateInstr(Instruction::kNegate, makePhyReg());
}

// kNegate R i
TEST_F(LIRABITest, TestkNegate_OutPhyReg_Imm) {
  translateInstr(Instruction::kNegate, makeOutPhyReg(), Imm{0});
  translateInstr(Instruction::kNegate, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Instruction::kNegate, makeOutPhyReg(), FPImm{0.0});
}

// kNegate R r
TEST_F(LIRABITest, TestkNegate_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kNegate, makeOutPhyReg(), makePhyReg());
}

// kNegate R m
TEST_F(LIRABITest, TestkNegate_OutPhyReg_Mem) {
  translateInstr(Instruction::kNegate, makeOutPhyReg(), makeStk());
}

// kInvert R i
TEST_F(LIRABITest, TestkInvert_OutPhyReg_Imm) {
  translateInstr(Instruction::kInvert, makeOutPhyReg(), Imm{0});
  translateInstr(Instruction::kInvert, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Instruction::kInvert, makeOutPhyReg(), FPImm{0.0});
}

// kInvert R r
TEST_F(LIRABITest, TestkInvert_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kInvert, makeOutPhyReg(), makePhyReg());
}

// kInvert R m
TEST_F(LIRABITest, TestkInvert_OutPhyReg_Mem) {
  translateInstr(Instruction::kInvert, makeOutPhyReg(), makeStk());
}

// kMovZX R r
TEST_F(LIRABITest, TestkMovZX_OutPhyReg_PhyReg) {
  translateInstr(
      Instruction::kMovZX,
      makeOutPhyReg(0, DataType::k64bit),
      makePhyReg(1, DataType::k32bit));
}

// kMovZX R m
TEST_F(LIRABITest, TestkMovZX_OutPhyReg_Mem) {
  translateInstr(
      Instruction::kMovZX,
      makeOutPhyReg(0, DataType::k64bit),
      makeStk(-16, DataType::k32bit));
}

// kMovSX R r
TEST_F(LIRABITest, TestkMovSX_OutPhyReg_PhyReg) {
  translateInstr(
      Instruction::kMovSX,
      makeOutPhyReg(0, DataType::k64bit),
      makePhyReg(1, DataType::k32bit));
}

// kMovSX R m
TEST_F(LIRABITest, TestkMovSX_OutPhyReg_Mem) {
  translateInstr(
      Instruction::kMovSX,
      makeOutPhyReg(0, DataType::k64bit),
      makeStk(-16, DataType::k32bit));
}

// kMovSXD R r
TEST_F(LIRABITest, TestkMovSXD_OutPhyReg_PhyReg) {
  translateInstr(
      Instruction::kMovSXD,
      makeOutPhyReg(0, DataType::k64bit),
      makePhyReg(1, DataType::k32bit));
}

// kMovSXD R m
TEST_F(LIRABITest, TestkMovSXD_OutPhyReg_Mem) {
  translateInstr(
      Instruction::kMovSXD,
      makeOutPhyReg(0, DataType::k64bit),
      makeStk(-16, DataType::k32bit));
}

// kUnreachable
TEST_F(LIRABITest, TestkUnreachable) {
  translateInstr(Instruction::kUnreachable);
}

// kAdd r i
TEST_F(LIRABITest, TestkAdd_PhyReg_Imm) {
  translateInstr(Instruction::kAdd, makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Instruction::kAdd, makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Instruction::kAdd, makePhyReg(), Imm{1 << 12});
#endif
}

// kAdd r r
TEST_F(LIRABITest, TestkAdd_PhyReg_PhyReg) {
  translateInstr(Instruction::kAdd, makePhyReg(), makePhyReg());
}

// kAdd r m
TEST_F(LIRABITest, TestkAdd_PhyReg_Mem) {
  translateInstr(Instruction::kAdd, makePhyReg(), makeStk());
}

// kAdd R r i
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kAdd, makeOutPhyReg(), makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(
      Instruction::kAdd, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(
      Instruction::kAdd, makeOutPhyReg(), makePhyReg(), Imm{1 << 12});
#endif
}

// kAdd R r r
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kAdd, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kAdd R r m
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kAdd, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kSub r i
TEST_F(LIRABITest, TestkSub_PhyReg_Imm) {
  translateInstr(Instruction::kSub, makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Instruction::kSub, makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Instruction::kSub, makePhyReg(), Imm{1 << 12});
#endif
}

// kSub r r
TEST_F(LIRABITest, TestkSub_PhyReg_PhyReg) {
  translateInstr(Instruction::kSub, makePhyReg(), makePhyReg());
}

// kSub r m
TEST_F(LIRABITest, TestkSub_PhyReg_Mem) {
  translateInstr(Instruction::kSub, makePhyReg(), makeStk());
}

// kSub R r i
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kSub, makeOutPhyReg(), makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(
      Instruction::kSub, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(
      Instruction::kSub, makeOutPhyReg(), makePhyReg(), Imm{1 << 12});
#endif
}

// kSub R r r
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kSub, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kSub R r m
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kSub, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kAnd r i
TEST_F(LIRABITest, TestkAnd_PhyReg_Imm) {
  translateInstr(Instruction::kAnd, makePhyReg(), Imm{1});
  translateInstr(Instruction::kAnd, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kAnd r r
TEST_F(LIRABITest, TestkAnd_PhyReg_PhyReg) {
  translateInstr(Instruction::kAnd, makePhyReg(), makePhyReg());
}

// kAnd r m
TEST_F(LIRABITest, TestkAnd_PhyReg_Mem) {
  translateInstr(Instruction::kAnd, makePhyReg(), makeStk());
}

// kAnd R r i
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kAnd, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Instruction::kAnd, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

// kAnd R r r
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kAnd, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kAnd R r m
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kAnd, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kOr r i
TEST_F(LIRABITest, TestkOr_PhyReg_Imm) {
  translateInstr(Instruction::kOr, makePhyReg(), Imm{1});
  translateInstr(Instruction::kOr, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kOr r r
TEST_F(LIRABITest, TestkOr_PhyReg_PhyReg) {
  translateInstr(Instruction::kOr, makePhyReg(), makePhyReg());
}

// kOr r m
TEST_F(LIRABITest, TestkOr_PhyReg_Mem) {
  translateInstr(Instruction::kOr, makePhyReg(), makeStk());
}

// kOr R r i
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kOr, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Instruction::kOr, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

// kOr R r r
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Instruction::kOr, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kOr R r m
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kOr, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kXor r i
TEST_F(LIRABITest, TestkXor_PhyReg_Imm) {
  translateInstr(Instruction::kXor, makePhyReg(), Imm{1});
  translateInstr(Instruction::kXor, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kXor r r
TEST_F(LIRABITest, TestkXor_PhyReg_PhyReg) {
  translateInstr(Instruction::kXor, makePhyReg(), makePhyReg());
}

// kXor r m
TEST_F(LIRABITest, TestkXor_PhyReg_Mem) {
  translateInstr(Instruction::kXor, makePhyReg(), makeStk());
}

// kXor R r i
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kXor, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Instruction::kXor, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

// kXor R r r
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kXor, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kXor R r m
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kXor, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kMul r i
TEST_F(LIRABITest, TestkMul_PhyReg_Imm) {
  translateInstr(Instruction::kMul, makePhyReg(), Imm{0});
  translateInstr(Instruction::kMul, makePhyReg(), Imm{UINT64_MAX});
}

// kMul r r
TEST_F(LIRABITest, TestkMul_PhyReg_PhyReg) {
  translateInstr(Instruction::kMul, makePhyReg(), makePhyReg());
}

// kMul r m
TEST_F(LIRABITest, TestkMul_PhyReg_Mem) {
  translateInstr(Instruction::kMul, makePhyReg(), makeStk());
}

// kMul R r i
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kMul, makeOutPhyReg(), makePhyReg(), Imm{0});
  translateInstr(
      Instruction::kMul, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
}

// kMul R r r
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kMul, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

// kMul R r m
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kMul, makeOutPhyReg(), makePhyReg(), makeStk());
}

// kDiv r r r
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kDiv, makePhyReg(0), makePhyReg(1), makePhyReg(2));
}

// kDiv r r m
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kDiv, makePhyReg(0), makePhyReg(1), makeStk());
}

// kDiv r r
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg) {
  translateInstr(Instruction::kDiv, makePhyReg(0), makePhyReg(1));
}

// kDiv r m
TEST_F(LIRABITest, TestkDiv_PhyReg_Mem) {
  translateInstr(Instruction::kDiv, makePhyReg(0), makeStk());
}

// kDivUn r r r
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kDivUn, makePhyReg(0), makePhyReg(1), makePhyReg(2));
}

// kDivUn r r m
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg_Mem) {
  translateInstr(Instruction::kDivUn, makePhyReg(0), makePhyReg(1), makeStk());
}

// kDivUn r r
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg) {
  translateInstr(Instruction::kDivUn, makePhyReg(0), makePhyReg(1));
}

// kDivUn r m
TEST_F(LIRABITest, TestkDivUn_PhyReg_Mem) {
  translateInstr(Instruction::kDivUn, makePhyReg(0), makeStk());
}

// kFadd X x x
TEST_F(LIRABITest, TestkFadd_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kFadd, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFadd x x
TEST_F(LIRABITest, TestkFadd_FPPhyReg_FPPhyReg) {
  translateInstr(Instruction::kFadd, makePhyRegFP(), makePhyRegFP());
}

// kFsub X x x
TEST_F(LIRABITest, TestkFsub_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kFsub, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFsub x x
TEST_F(LIRABITest, TestkFsub_FPPhyReg_FPPhyReg) {
  translateInstr(Instruction::kFsub, makePhyRegFP(), makePhyRegFP());
}

// kFmul X x x
TEST_F(LIRABITest, TestkFmul_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kFmul, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFmul x x
TEST_F(LIRABITest, TestkFmul_FPPhyReg_FPPhyReg) {
  translateInstr(Instruction::kFmul, makePhyRegFP(), makePhyRegFP());
}

// kFdiv X x x
TEST_F(LIRABITest, TestkFdiv_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kFdiv, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFdiv x x
TEST_F(LIRABITest, TestkFdiv_FPPhyReg_FPPhyReg) {
  translateInstr(Instruction::kFdiv, makePhyRegFP(), makePhyRegFP());
}

// kPush r
TEST_F(LIRABITest, TestkPush_PhyReg) {
  translateInstr(Instruction::kPush, makePhyReg());
}

// kPush m
TEST_F(LIRABITest, TestkPush_Mem) {
  translateInstr(Instruction::kPush, makeStk());
}

// kPush i
TEST_F(LIRABITest, TestkPush_Imm) {
  translateInstr(Instruction::kPush, Imm{0});
  translateInstr(Instruction::kPush, Imm{UINT64_MAX});
  translateInstr(Instruction::kPush, FPImm{0.0});
}

// kPop R
TEST_F(LIRABITest, TestkPop_OutPhyReg) {
  translateInstr(Instruction::kPop, makeOutPhyReg());
}

// kPop M
TEST_F(LIRABITest, TestkPop_Mem) {
  translateInstr(Instruction::kPop, makeOutStk());
}

#if defined(CINDER_X86_64)
// kCdq R r
TEST_F(LIRABITest, TestkCdq_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kCdq, makeOutPhyReg(), makePhyReg());
}

// kCwd R r
TEST_F(LIRABITest, TestkCwd_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kCwd, makeOutPhyReg(), makePhyReg());
}

// kCqo R r
TEST_F(LIRABITest, TestkCqo_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kCqo, makeOutPhyReg(), makePhyReg());
}
#endif

// kExchange R r
TEST_F(LIRABITest, TestkExchange_OutPhyReg_PhyReg) {
  translateInstr(Instruction::kExchange, makeOutPhyReg(), makePhyReg());
}

// kExchange X x
TEST_F(LIRABITest, TestkExchange_OutFPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kExchange,
      makeOutPhyRegFP(VECD_REG_BASE),
      makePhyRegFP(VECD_REG_BASE + 1));
}

// kCmp r r
TEST_F(LIRABITest, TestkCmp_PhyReg_PhyReg) {
  translateInstr(Instruction::kCmp, makePhyReg(0), makePhyReg(1));
}

// kCmp r i
TEST_F(LIRABITest, TestkCmp_PhyReg_Imm) {
  translateInstr(Instruction::kCmp, makePhyReg(0), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Instruction::kCmp, makePhyReg(0), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Instruction::kCmp, makePhyReg(0), Imm{1 << 12});
#endif
}

// kCmp x x
TEST_F(LIRABITest, TestkCmp_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kCmp,
      makePhyRegFP(VECD_REG_BASE),
      makePhyRegFP(VECD_REG_BASE + 1));
}

// kTest r r
TEST_F(LIRABITest, TestkTest_PhyReg_PhyReg) {
  translateInstr(Instruction::kTest, makePhyReg(0), makePhyReg(1));
}

// kTest32
TEST_F(LIRABITest, TestkTest32_PhyReg_PhyReg) {
  translateInstr(Instruction::kTest32, makePhyReg(0), makePhyReg(1));
}

// kBranch* b
TEST_F(LIRABITest, TestkBranch_Label) {
  translateInstr(Instruction::kBranch);
  translateInstr(Instruction::kBranchZ);
  translateInstr(Instruction::kBranchNZ);
  translateInstr(Instruction::kBranchA);
  translateInstr(Instruction::kBranchB);
  translateInstr(Instruction::kBranchAE);
  translateInstr(Instruction::kBranchBE);
  translateInstr(Instruction::kBranchG);
  translateInstr(Instruction::kBranchL);
  translateInstr(Instruction::kBranchGE);
  translateInstr(Instruction::kBranchLE);
  translateInstr(Instruction::kBranchC);
  translateInstr(Instruction::kBranchNC);
  translateInstr(Instruction::kBranchO);
  translateInstr(Instruction::kBranchNO);
  translateInstr(Instruction::kBranchS);
  translateInstr(Instruction::kBranchNS);
  translateInstr(Instruction::kBranchE);
  translateInstr(Instruction::kBranchNE);
}

// kEqual R r r
TEST_F(LIRABITest, TestkEqual_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kEqual, makeOutPhyReg(0), makePhyReg(1), makePhyReg(2));
}

// kEqual R r i
TEST_F(LIRABITest, TestkEqual_OutPhyReg_PhyReg_Imm) {
  translateInstr(Instruction::kEqual, makeOutPhyReg(0), makePhyReg(1), Imm{0});
  translateInstr(
      Instruction::kEqual, makeOutPhyReg(0), makePhyReg(1), Imm{UINT64_MAX});
}

// kEqual R r m
TEST_F(LIRABITest, TestkEqual_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kEqual, makeOutPhyReg(0), makePhyReg(1), makeImmPtr());
}

// kEqual R x x
TEST_F(LIRABITest, TestkEqual_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kEqual, makeOutPhyReg(), makePhyRegFP(), makePhyRegFP());
}

// kNotEqual R r r
TEST_F(LIRABITest, TestkNotEqual_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kNotEqual, makeOutPhyReg(0), makePhyReg(1), makePhyReg(2));
}

// kNotEqual R r i
TEST_F(LIRABITest, TestkNotEqual_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kNotEqual, makeOutPhyReg(0), makePhyReg(1), Imm{0});
  translateInstr(
      Instruction::kNotEqual, makeOutPhyReg(0), makePhyReg(1), Imm{UINT64_MAX});
}

// kNotEqual R r m
TEST_F(LIRABITest, TestkNotEqual_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kNotEqual, makeOutPhyReg(0), makePhyReg(1), makeImmPtr());
}

// kNotEqual R x x
TEST_F(LIRABITest, TestkNotEqual_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kNotEqual, makeOutPhyReg(), makePhyRegFP(), makePhyRegFP());
}

// kGreaterThanUnsigned R r r
TEST_F(LIRABITest, TestkGreaterThanUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kGreaterThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kGreaterThanUnsigned R r i
TEST_F(LIRABITest, TestkGreaterThanUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kGreaterThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Instruction::kGreaterThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kGreaterThanUnsigned R r m
TEST_F(LIRABITest, TestkGreaterThanUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kGreaterThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kGreaterThanUnsigned R x x
TEST_F(LIRABITest, TestkGreaterThanUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kGreaterThanUnsigned,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kGreaterThanEqualUnsigned R r r
TEST_F(LIRABITest, TestkGreaterThanEqualUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kGreaterThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kGreaterThanEqualUnsigned R r i
TEST_F(LIRABITest, TestkGreaterThanEqualUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kGreaterThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Instruction::kGreaterThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kGreaterThanEqualUnsigned R r m
TEST_F(LIRABITest, TestkGreaterThanEqualUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kGreaterThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kGreaterThanEqualUnsigned R x x
TEST_F(LIRABITest, TestkGreaterThanEqualUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kGreaterThanEqualUnsigned,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kLessThanUnsigned R r r
TEST_F(LIRABITest, TestkLessThanUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kLessThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kLessThanUnsigned R r i
TEST_F(LIRABITest, TestkLessThanUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kLessThanUnsigned, makeOutPhyReg(0), makePhyReg(1), Imm{0});
  translateInstr(
      Instruction::kLessThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kLessThanUnsigned R r m
TEST_F(LIRABITest, TestkLessThanUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kLessThanUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kLessThanUnsigned R x x
TEST_F(LIRABITest, TestkLessThanUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kLessThanUnsigned,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kLessThanEqualUnsigned R r r
TEST_F(LIRABITest, TestkLessThanEqualUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kLessThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kLessThanEqualUnsigned R r i
TEST_F(LIRABITest, TestkLessThanEqualUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kLessThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Instruction::kLessThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kLessThanEqualUnsigned R r m
TEST_F(LIRABITest, TestkLessThanEqualUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kLessThanEqualUnsigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kLessThanEqualUnsigned R x x
TEST_F(LIRABITest, TestkLessThanEqualUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Instruction::kLessThanEqualUnsigned,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kGreaterThanSigned R r r
TEST_F(LIRABITest, TestkGreaterThanSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kGreaterThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kGreaterThanSigned R r i
TEST_F(LIRABITest, TestkGreaterThanSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kGreaterThanSigned, makeOutPhyReg(0), makePhyReg(1), Imm{0});
  translateInstr(
      Instruction::kGreaterThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kGreaterThanSigned R r m
TEST_F(LIRABITest, TestkGreaterThanSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kGreaterThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kGreaterThanEqualSigned R r r
TEST_F(LIRABITest, TestkGreaterThanEqualSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kGreaterThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kGreaterThanEqualSigned R r i
TEST_F(LIRABITest, TestkGreaterThanEqualSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kGreaterThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Instruction::kGreaterThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kGreaterThanEqualSigned R r m
TEST_F(LIRABITest, TestkGreaterThanEqualSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kGreaterThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kLessThanSigned R r r
TEST_F(LIRABITest, TestkLessThanSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kLessThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kLessThanSigned R r i
TEST_F(LIRABITest, TestkLessThanSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kLessThanSigned, makeOutPhyReg(0), makePhyReg(1), Imm{0});
  translateInstr(
      Instruction::kLessThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kLessThanSigned R r m
TEST_F(LIRABITest, TestkLessThanSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kLessThanSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kLessThanEqualSigned R r r
TEST_F(LIRABITest, TestkLessThanEqualSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Instruction::kLessThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kLessThanEqualSigned R r i
TEST_F(LIRABITest, TestkLessThanEqualSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kLessThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Instruction::kLessThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

// kLessThanEqualSigned R r m
TEST_F(LIRABITest, TestkLessThanEqualSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Instruction::kLessThanEqualSigned,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}

// kInc r
TEST_F(LIRABITest, TestkInc_PhyReg) {
  translateInstr(Instruction::kInc, makePhyReg());
}

// kInc m
TEST_F(LIRABITest, TestkInc_Mem) {
  translateInstr(Instruction::kInc, makeStk());
}

// kDec r
TEST_F(LIRABITest, TestkDec_PhyReg) {
  translateInstr(Instruction::kDec, makePhyReg());
}

// kDec m
TEST_F(LIRABITest, TestkDec_Mem) {
  translateInstr(Instruction::kDec, makeStk());
}

// kBitTest r i
TEST_F(LIRABITest, TestkBitTest_PhyReg_PhyReg) {
  translateInstr(Instruction::kBitTest, makePhyReg(0), Imm{0});
  translateInstr(Instruction::kBitTest, makePhyReg(0), Imm{63});
}

// kYieldInitial ANY
TEST_F(LIRABITest, TestkYieldInitial) {
  PyCodeObject code;
  hir::FrameState frameState(BorrowedRef(&code), nullptr, nullptr, nullptr);

  hir::Register out(0);
  auto origin = std::unique_ptr<hir::InitialYield>(
      hir::InitialYield::create(&out, frameState));

  auto tstate = makeStk(-16);
  auto live_regs = Imm{0};
  auto deopt_idx = Imm{0};

  translateInstrWithOrigin(
      Instruction::kYieldInitial, origin.get(), tstate, live_regs, deopt_idx);
}

// kYieldFrom ANY
TEST_F(LIRABITest, TestkYieldFrom) {
  auto tstate = makeStk(-16);
  auto iter_slot = makeStk(-32);
  auto live_regs = Imm{0};
  auto deopt_idx = Imm{0};

  translateInstr(
      Instruction::kYieldFrom,
      tstate,
      makeStk(-48),
      iter_slot,
      live_regs,
      deopt_idx);

#if PY_VERSION_HEX >= 0x030C0000
  translateInstr(
      Instruction::kYieldFrom, tstate, Imm{0}, iter_slot, live_regs, deopt_idx);
#endif
}

// kYieldFromSkipInitialSend ANY
TEST_F(LIRABITest, TestkYieldFromSkipInitialSend) {
  auto tstate = makeStk(-16);
  auto send_value = makeStk(-32);
  auto iter_slot = makeStk(-48);
  auto live_regs = Imm{0};
  auto deopt_idx = Imm{0};

  translateInstr(
      Instruction::kYieldFromSkipInitialSend,
      tstate,
      send_value,
      iter_slot,
      live_regs,
      deopt_idx);
}

// kYieldFromHandleStopAsyncIteration ANY
TEST_F(LIRABITest, TestkYieldFromHandleStopAsyncIteration) {
  auto tstate = makeStk(-16);
  auto send_value = makeStk(-32);
  auto iter_slot = makeStk(-48);
  auto live_regs = Imm{0};
  auto deopt_idx = Imm{0};

  translateInstr(
      Instruction::kYieldFromHandleStopAsyncIteration,
      tstate,
      send_value,
      iter_slot,
      live_regs,
      deopt_idx);
}

// kYieldValue ANY
TEST_F(LIRABITest, TestkYieldValue) {
  auto tstate = makeStk(-16);
  auto live_regs = Imm{0};
  auto deopt_idx = Imm{0};

  translateInstr(
      Instruction::kYieldValue, tstate, Imm{0}, live_regs, deopt_idx);
  translateInstr(
      Instruction::kYieldValue, tstate, makeStk(-32), live_regs, deopt_idx);
}

// kSelect R r r i
TEST_F(LIRABITest, TestkSelect_OutPhyReg_PhyReg_PhyReg_Imm) {
  translateInstr(
      Instruction::kSelect,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2),
      Imm{0});
  translateInstr(
      Instruction::kSelect,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2),
      Imm{UINT64_MAX});
}

// kIntToBool R r
TEST_F(LIRABITest, TestkIntToBool_OutPhyReg_PhyReg) {
  translateInstr(
      Instruction::kIntToBool,
      makeOutPhyReg(0, DataType::k8bit),
      makePhyReg(1));
}

// kIntToBool R i
TEST_F(LIRABITest, TestkIntToBool_OutPhyReg_Imm) {
  translateInstr(
      Instruction::kIntToBool, makeOutPhyReg(0, DataType::k8bit), Imm{0});
  translateInstr(
      Instruction::kIntToBool,
      makeOutPhyReg(0, DataType::k8bit),
      Imm{UINT64_MAX});
}

} // namespace jit::lir
