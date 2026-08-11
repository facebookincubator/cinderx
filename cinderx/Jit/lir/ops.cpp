// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/ops.h"

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
    case Opcode::kMovSXD:
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
    case Opcode::kMovSXD:
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
