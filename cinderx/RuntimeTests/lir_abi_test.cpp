// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace asmjit;
using namespace cinderx::jit;
using namespace cinderx::jit::codegen;

namespace cinderx::jit::lir {

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
      Opcode opcode,
      const hir::Instr* origin,
      T&&... args) {
    hir::Function hirFunction;

    Environ environ;
    environ.ctx = getContext();
    environ.code_rt = environ.ctx->allocateCodeRuntime(
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
      case Opcode::kBranch:
        // kBranch supports both Label and MemoryIndirect operands. Only add
        // a label if no operands were already provided (i.e., the caller did
        // not pass an Ind operand).
        if (insn->getNumInputs() == 0) {
          environ.block_label_map.emplace(&bb, as.newLabel());
          insn->addOperands(Lbl{&bb});
        }
        break;
      case Opcode::kBranchCC:
      case Opcode::kBranchBitSet:
      case Opcode::kBranchBitNotSet:
        environ.block_label_map.emplace(&bb, as.newLabel());
        insn->addOperands(Lbl{&bb});
        break;
      case Opcode::kDeoptPatchpoint:
      case Opcode::kGuard: {
        environ.code_rt->addRawDeoptMetadata(DeoptMetadata{});
        // Create a dummy deopt exit block for the translator to look up.
        auto* deopt_bb = function.allocateBasicBlock();
        environ.deopt_exit_blocks[0] = deopt_bb;
        environ.block_label_map.emplace(deopt_bb, as.newLabel());
        break;
      }
      default:
        break;
    }

    // Translate the instruction using the auto translator.
    autogen::AutoTranslator::getInstance().translateInstr(&environ, insn);
  }

  template <typename... T>
  void translateInstr(Opcode opcode, T&&... args) {
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

TEST_F(LIRABITest, TestMemImmAndOutMemImmPreserveDataType) {
  Function function;
  BasicBlock bb(&function);

  auto* load = bb.allocateInstr(
      Opcode::kMove,
      nullptr,
      makeOutPhyReg(),
      MemImm{nullptr, DataType::k8bit});
  EXPECT_EQ(load->getInput(0)->sizeInBits(), bitSize(DataType::k8bit));

  auto* store = bb.allocateInstr(
      Opcode::kMove,
      nullptr,
      OutMemImm{nullptr, DataType::k8bit},
      makePhyReg(1, DataType::k8bit));
  EXPECT_EQ(store->output()->sizeInBits(), bitSize(DataType::k8bit));
}

// kLea R m
TEST_F(LIRABITest, TestkLea_OutPhyReg_Mem) {
  translateInstr(Opcode::kLea, makeOutPhyReg(), makeStk());
  translateInstr(Opcode::kLea, makeOutPhyReg(), MemImm{nullptr});
  translateInstr(Opcode::kLea, makeOutPhyReg(), makeInd(1, 16));
  translateInstr(Opcode::kLea, makeOutPhyReg(), makeIndScale(1, 2, 8, 16));
}

// kCall R i
#if !defined(CINDER_AARCH64)
TEST_F(LIRABITest, TestkCall_OutPhyReg_Imm) {
  translateInstr(Opcode::kCall, makeOutPhyReg(), makeImmPtr());
}
#endif

// kCall R r
TEST_F(LIRABITest, TestkCall_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kCall, makeOutPhyReg(), makePhyReg());
}

// kCall i
#if !defined(CINDER_AARCH64)
TEST_F(LIRABITest, TestkCall_Imm) {
  translateInstr(Opcode::kCall, makeImmPtr());
}
#endif

// kCall r
TEST_F(LIRABITest, TestkCall_PhyReg) {
  translateInstr(Opcode::kCall, makePhyReg());
}

#if defined(CINDER_AARCH64)
TEST_F(LIRABITest, TestkStorePair_SPBase) {
  hir::Function hir_function;

  Environ environ;
  environ.ctx = getContext();
  environ.code_rt = environ.ctx->allocateCodeRuntime(
      hir_function.code.get(),
      hir_function.builtins.get(),
      hir_function.globals.get());

  auto code_allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());

  CodeHolder code;
  code.init(code_allocator->asmJitEnvironment());

  arch::Builder as(&code);
  environ.as = &as;

  Function function;
  BasicBlock bb(&function);
  auto* instr = bb.allocateInstr(
      Opcode::kStorePair,
      nullptr,
      Imm{24},
      PhyReg{arch::reg_stack_pointer_loc, DataType::k64bit},
      PhyReg{X25, DataType::k64bit},
      PhyReg{X20, DataType::k64bit});

  autogen::AutoTranslator::getInstance().translateInstr(&environ, instr);

  EXPECT_EQ(as.finalize(), asmjit::kErrorOk);
  EXPECT_EQ(code.textSection()->bufferSize(), 4);
}
#endif

TEST_F(LIRABITest, TestkCall_FillsCallSiteLiveValueLocations) {
  if constexpr (!kFreeThreadedBuild) {
    SKIP(
        "Callsite live-value locations are only filled in free-threaded "
        "builds");
    return;
  }

  hir::Function hir_function;

  Environ environ;
  environ.ctx = getContext();
  environ.code_rt = environ.ctx->allocateCodeRuntime(
      hir_function.code.get(),
      hir_function.builtins.get(),
      hir_function.globals.get());

  std::unique_ptr<ICodeAllocator> code_allocator{CodeAllocator::make()};

  CodeHolder code;
  code.init(code_allocator->asmJitEnvironment());

  arch::Builder as(&code);
  environ.as = &as;

  Function function;
  BasicBlock bb(&function);

  const PhyLocation kRegisterLocation{ARGUMENT_REGS[0].loc, 64};
  const PhyLocation kStackLocation{-16, 64};

  Instruction* call = bb.allocateInstr(
      Opcode::kCall, nullptr, PhyReg{arch::reg_general_return_loc});
  Instruction* live_values = bb.allocateInstr(
      Opcode::kCallSiteLiveValues,
      nullptr,
      PhyReg{kRegisterLocation, DataType::kObject},
      Stk{kStackLocation, DataType::kObject});

  DeoptMetadata metadata;
  metadata.live_values = {
      LiveValue{
          PhyLocation{},
          hir::RefKind::kOwned,
          hir::ValueKind::kObject,
          LiveValue::Source::kUnknown},
      LiveValue{
          PhyLocation{},
          hir::RefKind::kOwned,
          hir::ValueKind::kObject,
          LiveValue::Source::kUnknown}};
  std::size_t deopt_idx =
      environ.code_rt->addRawDeoptMetadata(std::move(metadata));
  environ.callsite_live_value_metadata.emplace(
      call, Environ::CallSiteLiveValueMetadata{deopt_idx, live_values});

  autogen::AutoTranslator::getInstance().translateInstr(&environ, call);

  const DeoptMetadata& filled_metadata =
      environ.code_rt->getDeoptMetadata(deopt_idx);
  EXPECT_EQ(filled_metadata.live_values[0].location, kRegisterLocation);
  EXPECT_EQ(filled_metadata.live_values[1].location, kStackLocation);
}

// kCall m
#if !defined(CINDER_AARCH64)
TEST_F(LIRABITest, TestkCall_Stk) {
  translateInstr(Opcode::kCall, makeStk());
}
#endif

// kMove R r
TEST_F(LIRABITest, TestkMove_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kMove, makeOutPhyReg(), makePhyReg());
}

// kMove R i
TEST_F(LIRABITest, TestkMove_OutPhyReg_Imm) {
  translateInstr(Opcode::kMove, makeOutPhyReg(), Imm{0});
  translateInstr(Opcode::kMove, makeOutPhyReg(), Imm{UINT32_MAX});
  translateInstr(Opcode::kMove, makeOutPhyReg(), Imm{UINT32_MAX + 1});
  translateInstr(Opcode::kMove, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Opcode::kMove, makeOutPhyReg(), FPImm{0.0});
}

// kMove R m
TEST_F(LIRABITest, TestkMove_OutPhyReg_Mem) {
  translateInstr(Opcode::kMove, makeOutPhyReg(), makeStk());
#if !defined(CINDER_AARCH64)
  translateInstr(Opcode::kMove, makeOutPhyReg(), MemImm{nullptr});
#endif
  translateInstr(Opcode::kMove, makeOutPhyReg(), makeInd(1, 16));
  translateInstr(Opcode::kMove, makeOutPhyReg(), makeIndScale(1, 2, 8, 16));
}

// kMove R x
TEST_F(LIRABITest, TestkMove_OutPhyReg_FPPhyReg) {
  translateInstr(Opcode::kMove, makeOutPhyReg(), makePhyRegFP());
}

// kMove M r
TEST_F(LIRABITest, TestkMove_Mem_PhyReg) {
  translateInstr(Opcode::kMove, makeOutStk(), makePhyReg());
  translateInstr(Opcode::kMove, OutMemImm{nullptr}, makePhyReg());
  translateInstr(Opcode::kMove, makeOutInd(1, 16), makePhyReg());
  translateInstr(Opcode::kMove, makeOutIndScale(1, 2, 8, 16), makePhyReg());
}

// kMove M i
TEST_F(LIRABITest, TestkMove_Mem_Imm) {
#if !defined(CINDER_AARCH64)
  translateInstr(Opcode::kMove, makeOutStk(), Imm{0});
  translateInstr(Opcode::kMove, makeOutStk(), Imm{UINT64_MAX});
#endif
  translateInstr(Opcode::kMove, OutMemImm{nullptr}, Imm{0});
  translateInstr(Opcode::kMove, OutMemImm{nullptr}, Imm{UINT64_MAX});
  translateInstr(Opcode::kMove, makeOutInd(1, 16), Imm{0});
  translateInstr(Opcode::kMove, makeOutInd(1, 16), Imm{UINT64_MAX});
  translateInstr(Opcode::kMove, makeOutIndScale(1, 2, 8, 16), Imm{0});
  translateInstr(Opcode::kMove, makeOutIndScale(1, 2, 8, 16), Imm{UINT64_MAX});
#if !defined(CINDER_AARCH64)
  translateInstr(Opcode::kMove, makeOutStk(), FPImm{0.0});
#endif
  translateInstr(Opcode::kMove, OutMemImm{nullptr}, FPImm{0.0});
  translateInstr(Opcode::kMove, makeOutInd(1, 16), FPImm{0.0});
  translateInstr(Opcode::kMove, makeOutIndScale(1, 2, 8, 16), FPImm{0.0});
}

// kMove M x
TEST_F(LIRABITest, TestkMove_Mem_FPPhyReg) {
  translateInstr(Opcode::kMove, makeOutStk(), makePhyRegFP());
  translateInstr(Opcode::kMove, OutMemImm{nullptr}, makePhyRegFP());
  translateInstr(Opcode::kMove, makeOutInd(1, 16), makePhyRegFP());
  translateInstr(Opcode::kMove, makeOutIndScale(1, 2, 8, 16), makePhyRegFP());
}

// kMove X x
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kMove, makeOutPhyRegFP(), makePhyRegFP(VECD_REG_BASE + 1));
}

// kMove X m
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_Mem) {
  translateInstr(Opcode::kMove, makeOutPhyRegFP(), makeStk());
#if !defined(CINDER_AARCH64)
  translateInstr(Opcode::kMove, makeOutPhyRegFP(), MemImm{nullptr});
#endif
  translateInstr(Opcode::kMove, makeOutPhyRegFP(), makeInd(1, 16));
  translateInstr(Opcode::kMove, makeOutPhyRegFP(), makeIndScale(1, 2, 8, 16));
}

// kMove X r
TEST_F(LIRABITest, TestkMove_OutFPPhyReg_PhyReg) {
  translateInstr(Opcode::kMove, makeOutPhyRegFP(), makePhyReg());
}

// kGuard ANY
TEST_F(LIRABITest, TestkGuard) {
  translateInstr(Opcode::kGuard, Imm{kAlwaysFail}, Imm{0}, Imm{0}, Imm{0});
#if !defined(CINDER_AARCH64)
  translateInstr(Opcode::kGuard, Imm{kHasType}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(
      Opcode::kGuard, Imm{kHasType}, Imm{0}, makePhyReg(), MemImm{nullptr});
  translateInstr(
      Opcode::kGuard, Imm{kHasType}, Imm{0}, makePhyReg(), makePhyReg());
#endif
  translateInstr(Opcode::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(
      Opcode::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), MemImm{nullptr});
  translateInstr(Opcode::kGuard, Imm{kIs}, Imm{0}, makePhyReg(), makePhyReg());
  translateInstr(
      Opcode::kGuard, Imm{kNotNegative}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(Opcode::kGuard, Imm{kNotZero}, Imm{0}, makePhyReg(), Imm{0});
  translateInstr(Opcode::kGuard, Imm{kZero}, Imm{0}, makePhyReg(), Imm{0});
}

// kDeoptPatchpoint ANY
TEST_F(LIRABITest, TestkDeoptPatchpoint) {
  jit::CodePatcher patcher;

  uint16_t value = 0xFF00;
  std::array<uint8_t, 2> bytes{0xEF, 0xBE};
  patcher.link(reinterpret_cast<uintptr_t>(&value), bytes);

  translateInstr(Opcode::kDeoptPatchpoint, MemImm{&patcher}, Imm{0});
}

// kNegate r
TEST_F(LIRABITest, TestkNegate_PhyReg) {
  translateInstr(Opcode::kNegate, makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kNegate R i
TEST_F(LIRABITest, TestkNegate_OutPhyReg_Imm) {
  translateInstr(Opcode::kNegate, makeOutPhyReg(), Imm{0});
  translateInstr(Opcode::kNegate, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Opcode::kNegate, makeOutPhyReg(), FPImm{0.0});
}
#endif

// kNegate R r
TEST_F(LIRABITest, TestkNegate_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kNegate, makeOutPhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kNegate R m
TEST_F(LIRABITest, TestkNegate_OutPhyReg_Mem) {
  translateInstr(Opcode::kNegate, makeOutPhyReg(), makeStk());
}

// kInvert R i
TEST_F(LIRABITest, TestkInvert_OutPhyReg_Imm) {
  translateInstr(Opcode::kInvert, makeOutPhyReg(), Imm{0});
  translateInstr(Opcode::kInvert, makeOutPhyReg(), Imm{UINT64_MAX});
  translateInstr(Opcode::kInvert, makeOutPhyReg(), FPImm{0.0});
}
#endif

// kInvert R r
TEST_F(LIRABITest, TestkInvert_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kInvert, makeOutPhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kInvert R m
TEST_F(LIRABITest, TestkInvert_OutPhyReg_Mem) {
  translateInstr(Opcode::kInvert, makeOutPhyReg(), makeStk());
}
#endif

// kZext R r
TEST_F(LIRABITest, TestkZext_OutPhyReg_PhyReg) {
  translateInstr(
      Opcode::kZext,
      makeOutPhyReg(0, DataType::k64bit),
      makePhyReg(1, DataType::k32bit));
}

// kZext R m
TEST_F(LIRABITest, TestkZext_OutPhyReg_Mem) {
  translateInstr(
      Opcode::kZext,
      makeOutPhyReg(0, DataType::k64bit),
      makeStk(-16, DataType::k32bit));
}

// kSext R r
TEST_F(LIRABITest, TestkSext_OutPhyReg_PhyReg) {
  translateInstr(
      Opcode::kSext,
      makeOutPhyReg(0, DataType::k64bit),
      makePhyReg(1, DataType::k32bit));
}

// kSext R m
TEST_F(LIRABITest, TestkSext_OutPhyReg_Mem) {
  translateInstr(
      Opcode::kSext,
      makeOutPhyReg(0, DataType::k64bit),
      makeStk(-16, DataType::k32bit));
}

// kUnreachable
TEST_F(LIRABITest, TestkUnreachable) {
  translateInstr(Opcode::kUnreachable);
}

// kAdd r i
TEST_F(LIRABITest, TestkAdd_PhyReg_Imm) {
  translateInstr(Opcode::kAdd, makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Opcode::kAdd, makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Opcode::kAdd, makePhyReg(), Imm{1 << 12});
#endif
}

// kAdd r r
TEST_F(LIRABITest, TestkAdd_PhyReg_PhyReg) {
  translateInstr(Opcode::kAdd, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kAdd r m
TEST_F(LIRABITest, TestkAdd_PhyReg_Mem) {
  translateInstr(Opcode::kAdd, makePhyReg(), makeStk());
}
#endif

// kAdd R r i
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kAdd, makeOutPhyReg(), makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Opcode::kAdd, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Opcode::kAdd, makeOutPhyReg(), makePhyReg(), Imm{1 << 12});
#endif
}

// kAdd R r r
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kAdd, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kAdd R r m
TEST_F(LIRABITest, TestkAdd_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kAdd, makeOutPhyReg(), makePhyReg(), makeStk());
}
#endif

// kSub r i
TEST_F(LIRABITest, TestkSub_PhyReg_Imm) {
  translateInstr(Opcode::kSub, makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Opcode::kSub, makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Opcode::kSub, makePhyReg(), Imm{1 << 12});
#endif
}

// kSub r r
TEST_F(LIRABITest, TestkSub_PhyReg_PhyReg) {
  translateInstr(Opcode::kSub, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kSub r m
TEST_F(LIRABITest, TestkSub_PhyReg_Mem) {
  translateInstr(Opcode::kSub, makePhyReg(), makeStk());
}
#endif

// kSub R r i
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kSub, makeOutPhyReg(), makePhyReg(), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Opcode::kSub, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Opcode::kSub, makeOutPhyReg(), makePhyReg(), Imm{1 << 12});
#endif
}

// kSub R r r
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kSub, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kSub R r m
TEST_F(LIRABITest, TestkSub_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kSub, makeOutPhyReg(), makePhyReg(), makeStk());
}
#endif

// kAnd r i
TEST_F(LIRABITest, TestkAnd_PhyReg_Imm) {
  translateInstr(Opcode::kAnd, makePhyReg(), Imm{1});
  translateInstr(Opcode::kAnd, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kAnd r r
TEST_F(LIRABITest, TestkAnd_PhyReg_PhyReg) {
  translateInstr(Opcode::kAnd, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kAnd r m
TEST_F(LIRABITest, TestkAnd_PhyReg_Mem) {
  translateInstr(Opcode::kAnd, makePhyReg(), makeStk());
}
#endif

// kAnd R r i
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kAnd, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Opcode::kAnd, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

// kAnd R r r
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kAnd, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kAnd R r m
TEST_F(LIRABITest, TestkAnd_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kAnd, makeOutPhyReg(), makePhyReg(), makeStk());
}
#endif

// kOr r i
TEST_F(LIRABITest, TestkOr_PhyReg_Imm) {
  translateInstr(Opcode::kOr, makePhyReg(), Imm{1});
  translateInstr(Opcode::kOr, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kOr r r
TEST_F(LIRABITest, TestkOr_PhyReg_PhyReg) {
  translateInstr(Opcode::kOr, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kOr r m
TEST_F(LIRABITest, TestkOr_PhyReg_Mem) {
  translateInstr(Opcode::kOr, makePhyReg(), makeStk());
}
#endif

// kOr R r i
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kOr, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Opcode::kOr, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

// kOr R r r
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kOr, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kOr R r m
TEST_F(LIRABITest, TestkOr_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kOr, makeOutPhyReg(), makePhyReg(), makeStk());
}
#endif

// kXor r i
TEST_F(LIRABITest, TestkXor_PhyReg_Imm) {
  translateInstr(Opcode::kXor, makePhyReg(), Imm{1});
  translateInstr(Opcode::kXor, makePhyReg(), Imm{UINT64_MAX - 1});
}

// kXor r r
TEST_F(LIRABITest, TestkXor_PhyReg_PhyReg) {
  translateInstr(Opcode::kXor, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kXor r m
TEST_F(LIRABITest, TestkXor_PhyReg_Mem) {
  translateInstr(Opcode::kXor, makePhyReg(), makeStk());
}
#endif

// kXor R r i
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kXor, makeOutPhyReg(), makePhyReg(), Imm{1});
  translateInstr(
      Opcode::kXor, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX - 1});
}

#if !defined(CINDER_AARCH64)
// kMul r i
TEST_F(LIRABITest, TestkMul_PhyReg_Imm) {
  translateInstr(Opcode::kMul, makePhyReg(), Imm{0});
  translateInstr(Opcode::kMul, makePhyReg(), Imm{UINT64_MAX});
}
#endif

// kXor R r r
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kXor, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kXor R r m
TEST_F(LIRABITest, TestkXor_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kXor, makeOutPhyReg(), makePhyReg(), makeStk());
}
#endif

// kMul r r
TEST_F(LIRABITest, TestkMul_PhyReg_PhyReg) {
  translateInstr(Opcode::kMul, makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kMul R r i
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_Imm) {
  translateInstr(Opcode::kMul, makeOutPhyReg(), makePhyReg(), Imm{0});
  translateInstr(Opcode::kMul, makeOutPhyReg(), makePhyReg(), Imm{UINT64_MAX});
}

// kMul r m
TEST_F(LIRABITest, TestkMul_PhyReg_Mem) {
  translateInstr(Opcode::kMul, makePhyReg(), makeStk());
}
#endif

// kMul R r r
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kMul, makeOutPhyReg(), makePhyReg(), makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kMul R r m
TEST_F(LIRABITest, TestkMul_OutPhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kMul, makeOutPhyReg(), makePhyReg(), makeStk());
}
#else

// kMulAdd R r r r
TEST_F(LIRABITest, TestkMulAdd_OutPhyReg_PhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kMulAdd,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2),
      makePhyReg(3));
}
#endif

// kDiv r r r
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kDiv, makePhyReg(0), makePhyReg(1), makePhyReg(2));
}

#if !defined(CINDER_AARCH64)
// kDiv r r m
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kDiv, makePhyReg(0), makePhyReg(1), makeStk());
}
#endif

// kDiv r r
TEST_F(LIRABITest, TestkDiv_PhyReg_PhyReg) {
  translateInstr(Opcode::kDiv, makePhyReg(0), makePhyReg(1));
}

#if !defined(CINDER_AARCH64)
// kDiv r m
TEST_F(LIRABITest, TestkDiv_PhyReg_Mem) {
  translateInstr(Opcode::kDiv, makePhyReg(0), makeStk());
}
#endif

// kDivUn r r r
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg_PhyReg) {
  translateInstr(Opcode::kDivUn, makePhyReg(0), makePhyReg(1), makePhyReg(2));
}

#if !defined(CINDER_AARCH64)
// kDivUn r r m
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg_Mem) {
  translateInstr(Opcode::kDivUn, makePhyReg(0), makePhyReg(1), makeStk());
}
#endif

// kDivUn r r
TEST_F(LIRABITest, TestkDivUn_PhyReg_PhyReg) {
  translateInstr(Opcode::kDivUn, makePhyReg(0), makePhyReg(1));
}

#if !defined(CINDER_AARCH64)
// kDivUn r m
TEST_F(LIRABITest, TestkDivUn_PhyReg_Mem) {
  translateInstr(Opcode::kDivUn, makePhyReg(0), makeStk());
}
#endif

// kFadd X x x
TEST_F(LIRABITest, TestkFadd_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kFadd, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFadd x x
TEST_F(LIRABITest, TestkFadd_FPPhyReg_FPPhyReg) {
  translateInstr(Opcode::kFadd, makePhyRegFP(), makePhyRegFP());
}

// kFsub X x x
TEST_F(LIRABITest, TestkFsub_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kFsub, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFsub x x
TEST_F(LIRABITest, TestkFsub_FPPhyReg_FPPhyReg) {
  translateInstr(Opcode::kFsub, makePhyRegFP(), makePhyRegFP());
}

// kFmul X x x
TEST_F(LIRABITest, TestkFmul_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kFmul, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFmul x x
TEST_F(LIRABITest, TestkFmul_FPPhyReg_FPPhyReg) {
  translateInstr(Opcode::kFmul, makePhyRegFP(), makePhyRegFP());
}

// kFdiv X x x
TEST_F(LIRABITest, TestkFdiv_OutFPPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kFdiv, makeOutPhyRegFP(), makePhyRegFP(), makePhyRegFP());
}

// kFdiv x x
TEST_F(LIRABITest, TestkFdiv_FPPhyReg_FPPhyReg) {
  translateInstr(Opcode::kFdiv, makePhyRegFP(), makePhyRegFP());
}

// kPush r
TEST_F(LIRABITest, TestkPush_PhyReg) {
  translateInstr(Opcode::kPush, makePhyReg());
}

// kPush m
TEST_F(LIRABITest, TestkPush_Mem) {
  translateInstr(Opcode::kPush, makeStk());
}

#if !defined(CINDER_AARCH64)
// kPush i
TEST_F(LIRABITest, TestkPush_Imm) {
  translateInstr(Opcode::kPush, Imm{0});
  translateInstr(Opcode::kPush, Imm{UINT64_MAX});
  translateInstr(Opcode::kPush, FPImm{0.0});
}
#endif

// kPop R
TEST_F(LIRABITest, TestkPop_OutPhyReg) {
  translateInstr(Opcode::kPop, makeOutPhyReg());
}

// kPop M
TEST_F(LIRABITest, TestkPop_Mem) {
  translateInstr(Opcode::kPop, makeOutStk());
}

#if defined(CINDER_X86_64)
// kX64Cdq R r
TEST_F(LIRABITest, TestkCdq_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kX64Cdq, makeOutPhyReg(), makePhyReg());
}

// kX64Cwd R r
TEST_F(LIRABITest, TestkCwd_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kX64Cwd, makeOutPhyReg(), makePhyReg());
}

// kX64Cqo R r
TEST_F(LIRABITest, TestkCqo_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kX64Cqo, makeOutPhyReg(), makePhyReg());
}
#endif

// kExchange R r
TEST_F(LIRABITest, TestkExchange_OutPhyReg_PhyReg) {
  translateInstr(Opcode::kExchange, makeOutPhyReg(), makePhyReg());
}

// kExchange X x
TEST_F(LIRABITest, TestkExchange_OutFPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kExchange,
      makeOutPhyRegFP(VECD_REG_BASE),
      makePhyRegFP(VECD_REG_BASE + 1));
}

// kCmp r r
TEST_F(LIRABITest, TestkCmp_PhyReg_PhyReg) {
  translateInstr(Opcode::kCmp, makePhyReg(0), makePhyReg(1));
}

// kCmp r i
TEST_F(LIRABITest, TestkCmp_PhyReg_Imm) {
  translateInstr(Opcode::kCmp, makePhyReg(0), Imm{0});

#if defined(CINDER_X86_64)
  translateInstr(Opcode::kCmp, makePhyReg(0), Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(Opcode::kCmp, makePhyReg(0), Imm{1 << 12});
#endif
}

// kCmp x x
TEST_F(LIRABITest, TestkCmp_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCmp,
      makePhyRegFP(VECD_REG_BASE),
      makePhyRegFP(VECD_REG_BASE + 1));
}

// kTest r r
TEST_F(LIRABITest, TestkTest_PhyReg_PhyReg) {
  translateInstr(Opcode::kTest, makePhyReg(0), makePhyReg(1));
}

// kTest32
TEST_F(LIRABITest, TestkTest32_PhyReg_PhyReg) {
  translateInstr(Opcode::kTest32, makePhyReg(0), makePhyReg(1));
}

// kBranchCC b
TEST_F(LIRABITest, TestkBranch_Label) {
  translateInstr(Opcode::kBranch);
  translateInstr(Opcode::kBranchCC, Condition::kZero);
  translateInstr(Opcode::kBranchCC, Condition::kNotZero);
  translateInstr(Opcode::kBranchCC, Condition::kUnsignedGT);
  translateInstr(Opcode::kBranchCC, Condition::kUnsignedLT);
  translateInstr(Opcode::kBranchCC, Condition::kUnsignedGE);
  translateInstr(Opcode::kBranchCC, Condition::kUnsignedLE);
  translateInstr(Opcode::kBranchCC, Condition::kSignedGT);
  translateInstr(Opcode::kBranchCC, Condition::kSignedLT);
  translateInstr(Opcode::kBranchCC, Condition::kSignedGE);
  translateInstr(Opcode::kBranchCC, Condition::kSignedLE);
  translateInstr(Opcode::kBranchCC, Condition::kCarry);
  translateInstr(Opcode::kBranchCC, Condition::kNotCarry);
  translateInstr(Opcode::kBranchCC, Condition::kOverflow);
  translateInstr(Opcode::kBranchCC, Condition::kNotOverflow);
  translateInstr(Opcode::kBranchCC, Condition::kSign);
  translateInstr(Opcode::kBranchCC, Condition::kNotSign);
  translateInstr(Opcode::kBranchCC, Condition::kEqual);
  translateInstr(Opcode::kBranchCC, Condition::kNotEqual);
}

// kBranch with MemoryIndirect (indirect jump)
TEST_F(LIRABITest, TestkBranch_Indirect) {
  translateInstr(Opcode::kBranch, Ind(ARGUMENT_REGS[0]));
  translateInstr(Opcode::kBranch, Ind(ARGUMENT_REGS[0], 8));
}

// kBranch with Imm (direct address jump)
TEST_F(LIRABITest, TestkBranch_Imm) {
  translateInstr(
      Opcode::kBranch, Imm{reinterpret_cast<uint64_t>(testImmPtrTarget)});
}

// kCompare<Equal> R r r
TEST_F(LIRABITest, TestkCompare_Equal_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<Equal> R r i
TEST_F(LIRABITest, TestkCompare_Equal_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<Equal> R r m
TEST_F(LIRABITest, TestkCompare_Equal_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<Equal> R x x
TEST_F(LIRABITest, TestkCompare_Equal_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kEqual,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<NotEqual> R r r
TEST_F(LIRABITest, TestkCompare_NotEqual_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kNotEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<NotEqual> R r i
TEST_F(LIRABITest, TestkCompare_NotEqual_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kNotEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kNotEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<NotEqual> R r m
TEST_F(LIRABITest, TestkCompare_NotEqual_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kNotEqual,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<NotEqual> R x x
TEST_F(LIRABITest, TestkCompare_NotEqual_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kNotEqual,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<GreaterThanUnsigned> R r r
TEST_F(LIRABITest, TestkCompare_GreaterThanUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<GreaterThanUnsigned> R r i
TEST_F(LIRABITest, TestkCompare_GreaterThanUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<GreaterThanUnsigned> R r m
TEST_F(LIRABITest, TestkCompare_GreaterThanUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<GreaterThanUnsigned> R x x
TEST_F(
    LIRABITest,
    TestkCompare_GreaterThanUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGT,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<GreaterThanEqualUnsigned> R r r
TEST_F(
    LIRABITest,
    TestkCompare_GreaterThanEqualUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<GreaterThanEqualUnsigned> R r i
TEST_F(LIRABITest, TestkCompare_GreaterThanEqualUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<GreaterThanEqualUnsigned> R r m
TEST_F(LIRABITest, TestkCompare_GreaterThanEqualUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<GreaterThanEqualUnsigned> R x x
TEST_F(
    LIRABITest,
    TestkCompare_GreaterThanEqualUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedGE,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<LessThanUnsigned> R r r
TEST_F(LIRABITest, TestkCompare_LessThanUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<LessThanUnsigned> R r i
TEST_F(LIRABITest, TestkCompare_LessThanUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<LessThanUnsigned> R r m
TEST_F(LIRABITest, TestkCompare_LessThanUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<LessThanUnsigned> R x x
TEST_F(LIRABITest, TestkCompare_LessThanUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLT,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<LessThanEqualUnsigned> R r r
TEST_F(LIRABITest, TestkCompare_LessThanEqualUnsigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<LessThanEqualUnsigned> R r i
TEST_F(LIRABITest, TestkCompare_LessThanEqualUnsigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<LessThanEqualUnsigned> R r m
TEST_F(LIRABITest, TestkCompare_LessThanEqualUnsigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<LessThanEqualUnsigned> R x x
TEST_F(
    LIRABITest,
    TestkCompare_LessThanEqualUnsigned_OutPhyReg_FPPhyReg_FPPhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kUnsignedLE,
      makeOutPhyReg(),
      makePhyRegFP(),
      makePhyRegFP());
}

// kCompare<GreaterThanSigned> R r r
TEST_F(LIRABITest, TestkCompare_GreaterThanSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<GreaterThanSigned> R r i
TEST_F(LIRABITest, TestkCompare_GreaterThanSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<GreaterThanSigned> R r m
TEST_F(LIRABITest, TestkCompare_GreaterThanSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<GreaterThanEqualSigned> R r r
TEST_F(
    LIRABITest,
    TestkCompare_GreaterThanEqualSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<GreaterThanEqualSigned> R r i
TEST_F(LIRABITest, TestkCompare_GreaterThanEqualSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<GreaterThanEqualSigned> R r m
TEST_F(LIRABITest, TestkCompare_GreaterThanEqualSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedGE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<LessThanSigned> R r r
TEST_F(LIRABITest, TestkCompare_LessThanSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<LessThanSigned> R r i
TEST_F(LIRABITest, TestkCompare_LessThanSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<LessThanSigned> R r m
TEST_F(LIRABITest, TestkCompare_LessThanSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLT,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kCompare<LessThanEqualSigned> R r r
TEST_F(LIRABITest, TestkCompare_LessThanEqualSigned_OutPhyReg_PhyReg_PhyReg) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2));
}

// kCompare<LessThanEqualSigned> R r i
TEST_F(LIRABITest, TestkCompare_LessThanEqualSigned_OutPhyReg_PhyReg_Imm) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{0});
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      Imm{UINT64_MAX});
}

#if !defined(CINDER_AARCH64)
// kCompare<LessThanEqualSigned> R r m
TEST_F(LIRABITest, TestkCompare_LessThanEqualSigned_OutPhyReg_PhyReg_Mem) {
  translateInstr(
      Opcode::kCompare,
      Condition::kSignedLE,
      makeOutPhyReg(0),
      makePhyReg(1),
      makeImmPtr());
}
#endif

// kInc r
TEST_F(LIRABITest, TestkInc_PhyReg) {
  translateInstr(Opcode::kInc, makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kInc m
TEST_F(LIRABITest, TestkInc_Mem) {
  translateInstr(Opcode::kInc, makeStk());
}
#endif

// kDec r
TEST_F(LIRABITest, TestkDec_PhyReg) {
  translateInstr(Opcode::kDec, makePhyReg());
}

#if !defined(CINDER_AARCH64)
// kDec m
TEST_F(LIRABITest, TestkDec_Mem) {
  translateInstr(Opcode::kDec, makeStk());
}
#endif

// kBranchBitSet r i l
TEST_F(LIRABITest, TestkBranchBitSet_PhyReg_Imm_Label) {
  translateInstr(Opcode::kBranchBitSet, makePhyReg(0), Imm{0});
  translateInstr(Opcode::kBranchBitSet, makePhyReg(0), Imm{63});
}

// kBranchBitNotSet r i l
TEST_F(LIRABITest, TestkBranchBitNotSet_PhyReg_Imm_Label) {
  translateInstr(Opcode::kBranchBitNotSet, makePhyReg(0), Imm{0});
  translateInstr(Opcode::kBranchBitNotSet, makePhyReg(0), Imm{63});
}

// kSelect R r r r
TEST_F(LIRABITest, TestkSelect_OutPhyReg_PhyReg_PhyReg_PhyReg) {
#if defined(CINDER_X86_64)
  translateInstr(
      Opcode::kSelect, makeOutPhyReg(0), makePhyReg(1), makePhyReg(2), Imm{0});
  translateInstr(
      Opcode::kSelect,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2),
      Imm{UINT64_MAX});
#elif defined(CINDER_AARCH64)
  translateInstr(
      Opcode::kSelect,
      makeOutPhyReg(0),
      makePhyReg(1),
      makePhyReg(2),
      makePhyReg(3));
#endif
}

// kIntToBool R r
TEST_F(LIRABITest, TestkIntToBool_OutPhyReg_PhyReg) {
  translateInstr(
      Opcode::kIntToBool, makeOutPhyReg(0, DataType::k8bit), makePhyReg(1));
}

#if !defined(CINDER_AARCH64)
// kIntToBool R i
TEST_F(LIRABITest, TestkIntToBool_OutPhyReg_Imm) {
  translateInstr(Opcode::kIntToBool, makeOutPhyReg(0, DataType::k8bit), Imm{0});
  translateInstr(
      Opcode::kIntToBool, makeOutPhyReg(0, DataType::k8bit), Imm{UINT64_MAX});
}
#endif

// kMoveRelaxed R m
TEST_F(LIRABITest, TestkMoveRelaxed_OutPhyReg_Mem) {
  translateInstr(Opcode::kMoveRelaxed, makeOutPhyReg(), makeStk());
  translateInstr(Opcode::kMoveRelaxed, makeOutPhyReg(), makeInd(1, 16));
}

// kMoveRelaxed M r
TEST_F(LIRABITest, TestkMoveRelaxed_Mem_PhyReg) {
  translateInstr(Opcode::kMoveRelaxed, makeOutStk(), makePhyReg());
  translateInstr(Opcode::kMoveRelaxed, makeOutInd(1, 16), makePhyReg());
}

// kMoveRelaxed M i
TEST_F(LIRABITest, TestkMoveRelaxed_Mem_Imm) {
  translateInstr(Opcode::kMoveRelaxed, makeOutInd(1, 16), Imm{0});
}

// kMoveRelaxed rejects reg <- reg (no memory operand)
TEST_F(LIRABITest, TestkMoveRelaxed_RejectsRegReg) {
  EXPECT_DEATH(
      translateInstr(Opcode::kMoveRelaxed, makeOutPhyReg(), makePhyReg()),
      "kMoveRelaxed only supports");
}

} // namespace cinderx::jit::lir
