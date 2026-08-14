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

bool isCompare(Opcode opcode) {
  switch (opcode) {
    case Opcode::kEqual:
    case Opcode::kGreaterThanEqualSigned:
    case Opcode::kGreaterThanEqualUnsigned:
    case Opcode::kGreaterThanSigned:
    case Opcode::kGreaterThanUnsigned:
    case Opcode::kLessThanEqualSigned:
    case Opcode::kLessThanEqualUnsigned:
    case Opcode::kLessThanSigned:
    case Opcode::kLessThanUnsigned:
    case Opcode::kNotEqual:
      return true;
    default:
      break;
  }
  return false;
}

bool isBranchCC(Opcode opcode) {
  switch (opcode) {
    case Opcode::kBranchA:
    case Opcode::kBranchAE:
    case Opcode::kBranchB:
    case Opcode::kBranchBE:
    case Opcode::kBranchC:
    case Opcode::kBranchE:
    case Opcode::kBranchG:
    case Opcode::kBranchGE:
    case Opcode::kBranchL:
    case Opcode::kBranchLE:
    case Opcode::kBranchNC:
    case Opcode::kBranchNE:
    case Opcode::kBranchNO:
    case Opcode::kBranchNS:
    case Opcode::kBranchNZ:
    case Opcode::kBranchO:
    case Opcode::kBranchS:
    case Opcode::kBranchZ:
      return true;
    default:
      break;
  }
  return false;
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

#define CASE_FLIP(A, B)  \
  case Opcode::k##A:     \
    return Opcode::k##B; \
  case Opcode::k##B:     \
    return Opcode::k##A;

Opcode negateBranchCC(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(BranchA, BranchBE)
    CASE_FLIP(BranchB, BranchAE)
    CASE_FLIP(BranchC, BranchNC)
    CASE_FLIP(BranchE, BranchNE)
    CASE_FLIP(BranchG, BranchLE)
    CASE_FLIP(BranchL, BranchGE)
    CASE_FLIP(BranchO, BranchNO)
    CASE_FLIP(BranchS, BranchNS)
    CASE_FLIP(BranchZ, BranchNZ)
    default:
      break;
  }
  JIT_THROW("Not a conditional branch opcode: {}", opname(opcode));
}

Opcode flipBranchCCDirection(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(BranchA, BranchB)
    CASE_FLIP(BranchAE, BranchBE)
    CASE_FLIP(BranchL, BranchG)
    CASE_FLIP(BranchLE, BranchGE)
    default:
      break;
  }
  JIT_THROW("Unable to flip branch condition for opcode: {}", opname(opcode));
}

Opcode flipComparisonDirection(Opcode opcode) {
  switch (opcode) {
    CASE_FLIP(GreaterThanEqualSigned, LessThanEqualSigned)
    CASE_FLIP(GreaterThanEqualUnsigned, LessThanEqualUnsigned)
    CASE_FLIP(GreaterThanSigned, LessThanSigned)
    CASE_FLIP(GreaterThanUnsigned, LessThanUnsigned)
    case Opcode::kEqual:
      return Opcode::kEqual;
    case Opcode::kNotEqual:
      return Opcode::kNotEqual;
    default:
      break;
  }
  JIT_THROW(
      "Unable to flip comparison direction for opcode: {}", opname(opcode));
}

#undef CASE_FLIP

Opcode compareToBranchCC(Opcode opcode) {
  switch (opcode) {
    case Opcode::kEqual:
      return Opcode::kBranchE;
    case Opcode::kNotEqual:
      return Opcode::kBranchNE;
    case Opcode::kGreaterThanUnsigned:
      return Opcode::kBranchA;
    case Opcode::kLessThanUnsigned:
      return Opcode::kBranchB;
    case Opcode::kGreaterThanEqualUnsigned:
      return Opcode::kBranchAE;
    case Opcode::kLessThanEqualUnsigned:
      return Opcode::kBranchBE;
    case Opcode::kGreaterThanSigned:
      return Opcode::kBranchG;
    case Opcode::kLessThanSigned:
      return Opcode::kBranchL;
    case Opcode::kGreaterThanEqualSigned:
      return Opcode::kBranchGE;
    case Opcode::kLessThanEqualSigned:
      return Opcode::kBranchLE;
    default:
      break;
  }
  JIT_THROW("Not a compare opcode, {}", opname(opcode));
}

bool writesFlags(Opcode opcode) {
  switch (opcode) {
    case Opcode::kBind:
    case Opcode::kBranch:
    case Opcode::kBranchA:
    case Opcode::kBranchAE:
    case Opcode::kBranchB:
    case Opcode::kBranchBE:
    case Opcode::kBranchC:
    case Opcode::kBranchE:
    case Opcode::kBranchG:
    case Opcode::kBranchGE:
    case Opcode::kBranchL:
    case Opcode::kBranchLE:
    case Opcode::kBranchNC:
    case Opcode::kBranchNE:
    case Opcode::kBranchNO:
    case Opcode::kBranchNS:
    case Opcode::kBranchNZ:
    case Opcode::kBranchO:
    case Opcode::kBranchS:
    case Opcode::kBranchToYieldExit:
    case Opcode::kBranchZ:
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
    case Opcode::kMovSX:
    case Opcode::kMovZX:
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
    case Opcode::kBranchA:
    case Opcode::kBranchAE:
    case Opcode::kBranchB:
    case Opcode::kBranchBE:
    case Opcode::kBranchC:
    case Opcode::kBranchE:
    case Opcode::kBranchG:
    case Opcode::kBranchGE:
    case Opcode::kBranchL:
    case Opcode::kBranchLE:
    case Opcode::kBranchNC:
    case Opcode::kBranchNE:
    case Opcode::kBranchNO:
    case Opcode::kBranchNS:
    case Opcode::kBranchNZ:
    case Opcode::kBranchO:
    case Opcode::kBranchS:
    case Opcode::kCmp:
    case Opcode::kCondBranch:
    case Opcode::kDec:
    case Opcode::kDiv:
    case Opcode::kDivUn:
    case Opcode::kEqual:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kGreaterThanEqualSigned:
    case Opcode::kGreaterThanEqualUnsigned:
    case Opcode::kGreaterThanSigned:
    case Opcode::kGreaterThanUnsigned:
    case Opcode::kInc:
    case Opcode::kInt64ToDouble:
    case Opcode::kIntToBool:
    case Opcode::kInvert:
    case Opcode::kLShift:
    case Opcode::kLea:
    case Opcode::kLessThanEqualSigned:
    case Opcode::kLessThanEqualUnsigned:
    case Opcode::kLessThanSigned:
    case Opcode::kLessThanUnsigned:
    case Opcode::kLoadArg:
    case Opcode::kLoadSecondCallResult:
    case Opcode::kLoadThreadState:
    case Opcode::kMovConstPool:
    case Opcode::kMovSX:
    case Opcode::kMovZX:
    case Opcode::kMove:
    case Opcode::kMoveRelaxed:
    case Opcode::kMul:
    case Opcode::kMulAdd:
    case Opcode::kNegate:
    case Opcode::kNop:
    case Opcode::kNotEqual:
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
    case Opcode::kEqual:
    case Opcode::kExchange:
    case Opcode::kFadd:
    case Opcode::kFdiv:
    case Opcode::kFmul:
    case Opcode::kFsub:
    case Opcode::kGreaterThanEqualSigned:
    case Opcode::kGreaterThanEqualUnsigned:
    case Opcode::kGreaterThanSigned:
    case Opcode::kGreaterThanUnsigned:
    case Opcode::kLea:
    case Opcode::kLessThanEqualSigned:
    case Opcode::kLessThanEqualUnsigned:
    case Opcode::kLessThanSigned:
    case Opcode::kLessThanUnsigned:
    case Opcode::kNotEqual:
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
