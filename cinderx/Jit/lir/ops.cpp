// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/ops.h"

#include "cinderx/Common/log.h"

namespace cinderx::jit::lir {

std::string_view opname(Opcode opcode) {
  switch (opcode) {
#define STRINGIFY(NAME) \
  case Opcode::k##NAME: \
    return #NAME;
    FOREACH_LIR_OPCODE(STRINGIFY)
#undef STRINGIFY
    default:
      break;
  }
  return "<unknown opcode>";
}

std::string_view conditionName(Condition cond) {
  switch (cond) {
#define STRINGIFY(NAME, ...) \
  case Condition::k##NAME:   \
    return #NAME;
    FOREACH_LIR_CONDITION(STRINGIFY)
#undef STRINGIFY
    default:
      break;
  }
  return "<invalid condition>";
}

Condition negate(Condition cond) {
  switch (cond) {
#define NEGATE(NAME, NEGATED, ...) \
  case Condition::k##NAME:         \
    return Condition::k##NEGATED;
    FOREACH_LIR_CONDITION(NEGATE)
#undef NEGATE
    default:
      break;
  }
  JIT_THROW("Cannot negate an invalid condition ({})", static_cast<int>(cond));
}

Condition swapOperands(Condition cond) {
  switch (cond) {
#define SWAP(NAME, NEGATED, SWAPPED, ...) \
  case Condition::k##NAME:                \
    return Condition::k##SWAPPED;
    FOREACH_LIR_CONDITION(SWAP)
#undef SWAP
    default:
      break;
  }
  JIT_THROW(
      "Cannot swap operands of an invalid condition ({})",
      static_cast<int>(cond));
}

bool isSignedCompare(Condition cond) {
  switch (cond) {
    case Condition::kSignedLT:
    case Condition::kSignedLE:
    case Condition::kSignedGT:
    case Condition::kSignedGE:
      return true;
    default:
      break;
  }
  return false;
}

bool carriesCondition(Opcode opcode) {
  return opcode == Opcode::kBranchCC || opcode == Opcode::kCompare;
}

std::string_view branchCCName(Condition cond) {
  switch (cond) {
#define BRANCH_NAME(NAME, NEGATED, SWAPPED, BRANCH) \
  case Condition::k##NAME:                          \
    return #BRANCH;
    FOREACH_LIR_CONDITION(BRANCH_NAME)
#undef BRANCH_NAME
    default:
      break;
  }
  return "<invalid condition>";
}

std::string_view compareName(Condition cond) {
  switch (cond) {
#define COMPARE_NAME(COMPARE, CONDITION) \
  case Condition::k##CONDITION:          \
    return #COMPARE;
    FOREACH_LIR_COMPARE(COMPARE_NAME)
#undef COMPARE_NAME
    default:
      break;
  }
  JIT_THROW("Condition {} cannot be compared for", conditionName(cond));
}

bool isCompare(Opcode opcode) {
  return opcode == Opcode::kCompare;
}

bool isBranchCC(Opcode opcode) {
  return opcode == Opcode::kBranchCC;
}

bool isCmpBranch(Opcode opcode) {
  return opcode == Opcode::kCmpBranchZero ||
      opcode == Opcode::kCmpBranchNonZero;
}

bool isAnyBranch(Opcode opcode) {
  return opcode == Opcode::kCondBranch || opcode == Opcode::kBranchBitSet ||
      opcode == Opcode::kBranchBitNotSet || isBranchCC(opcode) ||
      isCmpBranch(opcode);
}

bool isTerminator(Opcode opcode) {
  switch (opcode) {
    case Opcode::kBranchToYieldExit:
    case Opcode::kEpilogueEnd:
    case Opcode::kReturn:
      return true;
    default:
      break;
  }
  return false;
}

bool isDeoptExit(Opcode opcode) {
  switch (opcode) {
    case Opcode::kGuard:
    case Opcode::kDeoptPatchpoint:
#if defined(CINDER_AARCH64)
    case Opcode::kA64GuardCC:
#endif
      return true;
    default:
      break;
  }
  return false;
}

bool isAnyYield(Opcode opcode) {
  switch (opcode) {
    case Opcode::kStoreGenYieldFromPoint:
    case Opcode::kStoreGenYieldPoint:
      return true;
    default:
      break;
  }
  return false;
}

bool writesFlags(Opcode opcode) {
  switch (opcode) {
    case Opcode::kBind:
    case Opcode::kBranch:
    case Opcode::kBranchCC:
    case Opcode::kBranchToYieldExit:
    case Opcode::kCallSiteLiveValues:
    case Opcode::kCmpBranchNonZero:
    case Opcode::kCmpBranchZero:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kInt64ToDouble:
    case Opcode::kInvert:
    case Opcode::kLea:
    case Opcode::kLoadArg:
    case Opcode::kLoadPair:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kMovConstPool:
    case Opcode::kMove:
    case Opcode::kMoveRelaxed:
    case Opcode::kMulAdd:
    case Opcode::kNop:
    case Opcode::kPhi:
    case Opcode::kPop:
    case Opcode::kPush:
    case Opcode::kReserveStack:
    case Opcode::kSext:
    case Opcode::kStorePair:
    case Opcode::kUnreachable:
    case Opcode::kVariadicPush:
    case Opcode::kZext:
#if defined(CINDER_X86_64)
    case Opcode::kX64Cdq:
    case Opcode::kX64Cqo:
    case Opcode::kX64Cwd:
#endif
      return false;
    default:
      break;
  }
  return true;
}

bool isEssential(Opcode opcode) {
  switch (opcode) {
    case Opcode::kAdd:
    case Opcode::kAnd:
    case Opcode::kBind:
    case Opcode::kBranch:
    case Opcode::kBranchCC:
    case Opcode::kCmp:
    case Opcode::kCondBranch:
    case Opcode::kDec:
    case Opcode::kDiv:
    case Opcode::kDivUn:
    case Opcode::kCompare:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kInc:
    case Opcode::kInt64ToDouble:
    case Opcode::kIntToBool:
    case Opcode::kInvert:
    case Opcode::kLShift:
    case Opcode::kLea:
    case Opcode::kLoadArg:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kLoadThreadState:
    case Opcode::kMovConstPool:
    case Opcode::kMove:
    case Opcode::kMoveRelaxed:
    case Opcode::kMul:
    case Opcode::kMulAdd:
    case Opcode::kNegate:
    case Opcode::kNop:
    case Opcode::kOr:
    case Opcode::kPhi:
    case Opcode::kRShift:
    case Opcode::kRShiftUn:
    case Opcode::kReserveStack:
    case Opcode::kReturn:
    case Opcode::kSelect:
    case Opcode::kSext:
    case Opcode::kSub:
    case Opcode::kTest32:
    case Opcode::kTest:
    case Opcode::kVarArgCall:
    case Opcode::kXor:
    case Opcode::kZext:
      return false;
    default:
      break;
  }
  return true;
}

bool outputMustBeRegister(Opcode opcode) {
  // This was lifted from the previous FOREACH_INSTR_TYPE macro for consistency
  // in the short term, but it doesn't seem like any of these opcodes have
  // outputs in the first place.
  switch (opcode) {
    case Opcode::kBranchBitNotSet:
    case Opcode::kBranchBitSet:
    case Opcode::kBranchToYieldExit:
    case Opcode::kCallSiteLiveValues:
    case Opcode::kCmpBranchNonZero:
    case Opcode::kCmpBranchZero:
    case Opcode::kCondBranch:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kEpilogueEnd:
    case Opcode::kLeave:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kLoadThreadState:
    case Opcode::kPop:
    case Opcode::kPrologue:
    case Opcode::kResumeGenYield:
    case Opcode::kRet:
    case Opcode::kSetupFrame:
    case Opcode::kStoreGenYieldFromPoint:
    case Opcode::kStoreGenYieldPoint:
    case Opcode::kStorePair:
    case Opcode::kTest32:
    case Opcode::kTest:
    case Opcode::kUnreachable:
    case Opcode::kVariadicPush:
#if defined(CINDER_AARCH64)
    case Opcode::kA64GuardCC:
#endif
      return false;
    default:
      break;
  }
  return true;
}

bool inputMustBeRegister(Opcode opcode, size_t idx) {
  switch (opcode) {
    case Opcode::kAdd:
    case Opcode::kAnd:
    case Opcode::kBranchBitNotSet:
    case Opcode::kBranchBitSet:
    case Opcode::kCmpBranchNonZero:
    case Opcode::kCmpBranchZero:
    case Opcode::kCondBranch:
    case Opcode::kDiv:
    case Opcode::kDivUn:
    case Opcode::kInt64ToDouble:
    case Opcode::kIntToBool:
    case Opcode::kLShift:
    case Opcode::kMul:
    case Opcode::kOr:
    case Opcode::kRShift:
    case Opcode::kRShiftUn:
    case Opcode::kVarArgCall:
    case Opcode::kVectorCallTstate:
    case Opcode::kXor:
      return idx == 0;

    case Opcode::kCmp:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kCompare:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kLea:
    case Opcode::kSub:
    case Opcode::kTest32:
    case Opcode::kTest:
      return idx <= 1;

    case Opcode::kMulAdd:
    case Opcode::kSelect:
      return idx <= 2;

    case Opcode::kGuard:
      return idx >= 2 && idx <= 3;

    case Opcode::kLoadPair:
      return idx >= 1 && idx <= 2;

    case Opcode::kStorePair:
      return idx >= 1 && idx <= 3;

    default:
      break;
  }
  return false;
}

bool inputsLiveAcross(Opcode opcode) {
  switch (opcode) {
    case Opcode::kFdiv:
    case Opcode::kFsub:
    case Opcode::kSelect:
    case Opcode::kSub:
      return true;
    default:
      break;
  }
  return false;
}

OperandSizeType operandSizeType(Opcode opcode) {
  switch (opcode) {
    case Opcode::kCall:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kInt64ToDouble:
    case Opcode::kLea:
    case Opcode::kLoadArg:
    case Opcode::kMulAdd:
    case Opcode::kReserveStack:
    case Opcode::kVectorCallTstate:
      return OperandSizeType::kAlways64;
    case Opcode::kAdd:
    case Opcode::kAnd:
    case Opcode::kCmp:
    case Opcode::kInvert:
    case Opcode::kLShift:
    case Opcode::kMovConstPool:
    case Opcode::kMove:
    case Opcode::kMoveRelaxed:
    case Opcode::kMul:
    case Opcode::kNegate:
    case Opcode::kOr:
    case Opcode::kRShift:
    case Opcode::kRShiftUn:
    case Opcode::kSub:
    case Opcode::kXor:
      return OperandSizeType::kOut;
    default:
      break;
  }
  return OperandSizeType::kDefault;
}

} // namespace cinderx::jit::lir
