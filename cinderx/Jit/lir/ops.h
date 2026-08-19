// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/define.h"

#include <cstdint>
#include <string_view>

namespace cinderx::jit::lir {

// LIR opcodes shared between all target hardware platforms.
#define FOREACH_LIR_OPCODE_COMMON(X)                            \
  X(Add)                                                        \
  X(And)                                                        \
  /* Associates a physical register with a virtual register */  \
  X(Bind)                                                       \
  X(Branch)                                                     \
  /* Branch on the condition it carries, read from the flags */ \
  X(BranchCC)                                                   \
  X(BranchBitNotSet)                                            \
  X(BranchBitSet)                                               \
  X(BranchToYieldExit)                                          \
  X(Call)                                                       \
  /* Carries post-call liveness metadata but emits no code. */  \
  X(CallSiteLiveValues)                                         \
  X(Cmp)                                                        \
  X(CmpBranchNonZero)                                           \
  X(CmpBranchZero)                                              \
  /* Materialise the condition it carries into a bool output */ \
  X(Compare)                                                    \
  X(CondBranch)                                                 \
  X(Dec)                                                        \
  X(DeoptPatchpoint)                                            \
  X(Div)                                                        \
  X(DivUn)                                                      \
  X(EpilogueEnd)                                                \
  X(Exchange)                                                   \
  X(Fadd)                                                       \
  X(Fdiv)                                                       \
  X(Fmul)                                                       \
  X(Fsub)                                                       \
  X(Guard)                                                      \
  X(Inc)                                                        \
  X(Int64ToDouble)                                              \
  X(IntToBool)                                                  \
  X(Invert)                                                     \
  X(LShift)                                                     \
  X(Lea)                                                        \
  X(Leave)                                                      \
  X(LoadArg)                                                    \
  X(LoadPair)                                                   \
  X(LoadSecondCallResult)                                       \
  X(LoadThreadState)                                            \
  X(MovConstPool)                                               \
  X(Move)                                                       \
  X(MoveRelaxed)                                                \
  X(Mul)                                                        \
  X(MulAdd)                                                     \
  X(Negate)                                                     \
  X(Nop)                                                        \
  X(Or)                                                         \
  X(Phi)                                                        \
  X(Pop)                                                        \
  X(Prologue)                                                   \
  X(Push)                                                       \
  X(RShift)                                                     \
  X(RShiftUn)                                                   \
  X(ReserveStack)                                               \
  X(ResumeGenYield)                                             \
  X(Ret)                                                        \
  X(Return)                                                     \
  X(Select)                                                     \
  X(SetupFrame)                                                 \
  X(Sext)                                                       \
  X(StoreGenYieldFromPoint)                                     \
  X(StoreGenYieldPoint)                                         \
  X(StorePair)                                                  \
  X(Sub)                                                        \
  X(Test)                                                       \
  X(Test32)                                                     \
  X(Unreachable)                                                \
  X(VarArgCall)                                                 \
  X(VariadicPush)                                               \
  X(VectorCallTstate)                                           \
  X(Xor)                                                        \
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

enum class Opcode : uint16_t {
#define DECLARE_OPCODE(NAME) k##NAME,
  FOREACH_LIR_OPCODE(DECLARE_OPCODE)
#undef DECLARE_OPCODE
};

// The conditions a comparison can test, and that a conditional branch can read
// back out of the machine's status flags.
//
// The names deliberately match asmjit's CondCode aliases, which are spelled
// identically for x86-64 and aarch64.  That lets codegen map a Condition onto
// either target without a per-architecture table of its own.
//
// Columns are the condition, the one that holds exactly when it does not, the
// one testing the same relation with the compared operands swapped, and the
// name a BranchCC carrying it prints and parses under.  Conditions that test a
// single flag rather than a comparison have no swapped form.
//
//    CONDITION     NEGATED       SWAPPED       BRANCH NAME
#define FOREACH_LIR_CONDITION(X)                  \
  X(Equal, NotEqual, Equal, BranchE)              \
  X(NotEqual, Equal, NotEqual, BranchNE)          \
  X(Zero, NotZero, Zero, BranchZ)                 \
  X(NotZero, Zero, NotZero, BranchNZ)             \
  X(SignedLT, SignedGE, SignedGT, BranchL)        \
  X(SignedLE, SignedGT, SignedGE, BranchLE)       \
  X(SignedGT, SignedLE, SignedLT, BranchG)        \
  X(SignedGE, SignedLT, SignedLE, BranchGE)       \
  X(UnsignedLT, UnsignedGE, UnsignedGT, BranchB)  \
  X(UnsignedLE, UnsignedGT, UnsignedGE, BranchBE) \
  X(UnsignedGT, UnsignedLE, UnsignedLT, BranchA)  \
  X(UnsignedGE, UnsignedLT, UnsignedLE, BranchAE) \
  X(Carry, NotCarry, Invalid, BranchC)            \
  X(NotCarry, Carry, Invalid, BranchNC)           \
  X(Overflow, NotOverflow, Invalid, BranchO)      \
  X(NotOverflow, Overflow, Invalid, BranchNO)     \
  X(Sign, NotSign, Invalid, BranchS)              \
  X(NotSign, Sign, Invalid, BranchNS)

// The name a Compare carrying each condition prints and parses under.  Only
// the conditions that come from comparing two values have one.
//
//    COMPARE NAME               CONDITION
#define FOREACH_LIR_COMPARE(X)         \
  X(Equal, Equal)                      \
  X(NotEqual, NotEqual)                \
  X(LessThanSigned, SignedLT)          \
  X(LessThanEqualSigned, SignedLE)     \
  X(GreaterThanSigned, SignedGT)       \
  X(GreaterThanEqualSigned, SignedGE)  \
  X(LessThanUnsigned, UnsignedLT)      \
  X(LessThanEqualUnsigned, UnsignedLE) \
  X(GreaterThanUnsigned, UnsignedGT)   \
  X(GreaterThanEqualUnsigned, UnsignedGE)

enum class Condition : uint16_t {
#define DECLARE_CONDITION(NAME, ...) k##NAME,
  FOREACH_LIR_CONDITION(DECLARE_CONDITION)
#undef DECLARE_CONDITION
      kInvalid,
};

// Get the string name of a condition.  This is a null-terminated literal value.
std::string_view conditionName(Condition cond);

// The condition that holds exactly when the given one does not.
Condition negate(Condition cond);

// The condition testing the same relation with the compared operands swapped,
// e.g. A >= B -> B <= A.  Invalid for conditions that read a single flag.
Condition swapOperands(Condition cond);

// Whether the condition compares its operands as signed values.
bool isSignedCompare(Condition cond);

// Whether instructions with this opcode carry a condition.
bool carriesCondition(Opcode opcode);

// The names a BranchCC and a Compare carrying the given condition are spelled
// with.  These are the old per-condition opcode names, kept so that LIR reads
// and parses the same as it did before the condition became a field.
std::string_view branchCCName(Condition cond);
std::string_view compareName(Condition cond);

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

// Whether the opcode compares two values and writes a boolean output.
bool isCompare(Opcode opcode);

// Whether the opcode branches on the machine's status flags.
bool isBranchCC(Opcode opcode);

// Whether the opcode compares a value against zero and branches on the result.
bool isCmpBranch(Opcode opcode);

// Whether the opcode is any kind of conditional branch.
bool isAnyBranch(Opcode opcode);

// Whether the opcode ends a basic block by leaving the function.
bool isTerminator(Opcode opcode);

// Whether this instruction records the physical locations of deopt live values,
// i.e. its trailing inputs are read back by MemoryView at deopt time.  Yields
// do this too, but are covered by isAnyYield().
bool isDeoptExit(Opcode opcode);

// Whether the opcode is a generator yield point.
bool isAnyYield(Opcode opcode);

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
