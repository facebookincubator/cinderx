// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/define.h"

#include <string_view>

namespace cinderx::jit::lir {

// LIR opcodes shared between all target hardware platforms.
#define FOREACH_LIR_OPCODE_COMMON(X)                           \
  X(Add)                                                       \
  X(And)                                                       \
  /* Associates a physical register with a virtual register */ \
  X(Bind)                                                      \
  X(Branch)                                                    \
  X(BranchA)                                                   \
  X(BranchAE)                                                  \
  X(BranchB)                                                   \
  X(BranchBE)                                                  \
  X(BranchBitNotSet)                                           \
  X(BranchBitSet)                                              \
  X(BranchC)                                                   \
  X(BranchE)                                                   \
  X(BranchG)                                                   \
  X(BranchGE)                                                  \
  X(BranchL)                                                   \
  X(BranchLE)                                                  \
  X(BranchNC)                                                  \
  X(BranchNE)                                                  \
  X(BranchNO)                                                  \
  X(BranchNS)                                                  \
  X(BranchNZ)                                                  \
  X(BranchO)                                                   \
  X(BranchS)                                                   \
  X(BranchToYieldExit)                                         \
  X(BranchZ)                                                   \
  X(Call)                                                      \
  /* Carries post-call liveness metadata but emits no code. */ \
  X(CallSiteLiveValues)                                        \
  X(Cmp)                                                       \
  X(CmpBranchNonZero)                                          \
  X(CmpBranchZero)                                             \
  X(CondBranch)                                                \
  X(Dec)                                                       \
  X(DeoptPatchpoint)                                           \
  X(Div)                                                       \
  X(DivUn)                                                     \
  X(EpilogueEnd)                                               \
  X(Equal)                                                     \
  X(Exchange)                                                  \
  X(Fadd)                                                      \
  X(Fdiv)                                                      \
  X(Fmul)                                                      \
  X(Fsub)                                                      \
  X(GreaterThanEqualSigned)                                    \
  X(GreaterThanEqualUnsigned)                                  \
  X(GreaterThanSigned)                                         \
  X(GreaterThanUnsigned)                                       \
  X(Guard)                                                     \
  X(Inc)                                                       \
  X(Int64ToDouble)                                             \
  X(IntToBool)                                                 \
  X(Invert)                                                    \
  X(LShift)                                                    \
  X(Lea)                                                       \
  X(Leave)                                                     \
  X(LessThanEqualSigned)                                       \
  X(LessThanEqualUnsigned)                                     \
  X(LessThanSigned)                                            \
  X(LessThanUnsigned)                                          \
  X(LoadArg)                                                   \
  X(LoadPair)                                                  \
  X(LoadSecondCallResult)                                      \
  X(LoadThreadState)                                           \
  X(MovConstPool)                                              \
  X(MovSX)                                                     \
  X(MovSXD)                                                    \
  X(MovZX)                                                     \
  X(Move)                                                      \
  X(MoveRelaxed)                                               \
  X(Mul)                                                       \
  X(MulAdd)                                                    \
  X(Negate)                                                    \
  X(Nop)                                                       \
  X(NotEqual)                                                  \
  X(Or)                                                        \
  X(Phi)                                                       \
  X(Pop)                                                       \
  X(Prologue)                                                  \
  X(Push)                                                      \
  X(RShift)                                                    \
  X(RShiftUn)                                                  \
  X(ReserveStack)                                              \
  X(ResumeGenYield)                                            \
  X(Ret)                                                       \
  X(Return)                                                    \
  X(Select)                                                    \
  X(SetupFrame)                                                \
  X(Sext)                                                      \
  X(StoreGenYieldFromPoint)                                    \
  X(StoreGenYieldPoint)                                        \
  X(StorePair)                                                 \
  X(Sub)                                                       \
  X(Test)                                                      \
  X(Test32)                                                    \
  X(Unreachable)                                               \
  X(VarArgCall)                                                \
  X(VariadicPush)                                              \
  X(VectorCallTstate)                                          \
  X(Xor)                                                       \
  X(Zext)

// LIR opcodes exclusive to x86-64.
#define FOREACH_LIR_OPCODE_X86_64(X) \
  X(X64Cdq)                          \
  X(X64Cqo)                          \
  X(X64Cwd)

// LIR opcodes exclusive to aarch64.
#define FOREACH_LIR_OPCODE_AARCH64(X) X(A64GuardCC)

#if defined(CINDER_X86_64)
#define FOREACH_LIR_OPCODE(X)  \
  FOREACH_LIR_OPCODE_COMMON(X) \
  FOREACH_LIR_OPCODE_X86_64(X)
#elif defined(CINDER_AARCH64)
#define FOREACH_LIR_OPCODE(X)  \
  FOREACH_LIR_OPCODE_COMMON(X) \
  FOREACH_LIR_OPCODE_AARCH64(X)
#else
#define FOREACH_LIR_OPCODE FOREACH_LIR_OPCODE_COMMON
#endif

enum class Opcode {
#define DECLARE_OPCODE(NAME) k##NAME,
  FOREACH_LIR_OPCODE(DECLARE_OPCODE)
#undef DECLARE_OPCODE
};

// Describes how an LIR instruction's operand sizes are determined.
enum class OperandSizeType {
  // Every operand uses the size determined by its DataType.
  kDefault,

  // Every operand is 64 bits.
  kAlways64,

  // Every operand is the same size as the output, or the first input (when
  // there is no output).
  kOut,
};

// Get the string name of an opcode.  This is a null-terminated literal value.
std::string_view opname(Opcode opcode);

// Whether the opcode will modify the machine's status flags.
bool writesFlags(Opcode opcode);

// Whether the opcode has side effects and cannot be deleted.
//
// Note: Any instruction with no output must be essential.
bool isEssential(Opcode opcode);

// Whether the opcode has to have its output allocated to a physical register,
// and not a stack slot.
bool outputMustBeRegister(Opcode opcode);

// Whether the opcode has to have a specific input allocated to a physical
// register, and not a stack slot.
bool inputMustBeRegister(Opcode opcode, size_t idx);

// Whether the opcode's inputs are still live / available after the opcode has
// finished executing.
//
// When false, one of the inputs can be assigned to the same physical location
// as one of the inputs.
//
// When true, following instructions can continue to read the same input
// locations.
bool inputsLiveAcross(Opcode opcode);

// See OperandSizeType for details.
OperandSizeType operandSizeType(Opcode opcode);

} // namespace cinderx::jit::lir
