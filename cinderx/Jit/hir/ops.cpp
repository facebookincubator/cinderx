// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/ops.h"

#include "cinderx/Common/log.h"

namespace cinderx::jit::hir {

std::optional<Opcode> opcodeFromName(std::string_view name) {
#define CASE_OP(OP)       \
  if (name == #OP) {      \
    return Opcode::k##OP; \
  }
  FOREACH_OPCODE(CASE_OP)
#undef CASE_OP
  return std::nullopt;
}

size_t numOutputs(Opcode op) {
  switch (op) {
    case Opcode::kAtQuiescentState:
    case Opcode::kBatchDecref:
    case Opcode::kBeginInlinedFunction:
    case Opcode::kBranch:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kDecref:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteGlobal:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kEndGeneratorFrame:
    case Opcode::kEndInlinedFunction:
    case Opcode::kGuard:
    case Opcode::kHintType:
    case Opcode::kIncref:
    case Opcode::kInitFrameCellVars:
    case Opcode::kInitListElements:
    case Opcode::kInitTupleElements:
    case Opcode::kLoadFrame:
    case Opcode::kProfileUseTypes:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kReturn:
    case Opcode::kSetCellItem:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetFunctionAttr:
    case Opcode::kSnapshot:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreAttr:
    case Opcode::kStoreField:
    case Opcode::kStoreSubscr:
    case Opcode::kUnreachable:
    case Opcode::kUpdatePrevInstr:
    case Opcode::kUseObj:
    case Opcode::kUseType:
    case Opcode::kXDecref:
    case Opcode::kXIncref:
      return 0;
    case Opcode::kAssign:
    case Opcode::kBinaryOp:
    case Opcode::kBitCast:
    case Opcode::kBuildInterpolation:
    case Opcode::kBuildSlice:
    case Opcode::kBuildString:
    case Opcode::kBuildTemplate:
    case Opcode::kCIntToCBool:
    case Opcode::kCallCFunc:
    case Opcode::kCallEx:
    case Opcode::kCallIntrinsic:
    case Opcode::kCallMethod:
    case Opcode::kCallStatic:
    case Opcode::kCast:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCheckVar:
    case Opcode::kCompare:
    case Opcode::kCompareBool:
    case Opcode::kConvertValue:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDictMerge:
    case Opcode::kDictSubscr:
    case Opcode::kDictUpdate:
    case Opcode::kDoubleBinaryOp:
    case Opcode::kEagerImportName:
    case Opcode::kFillTypeAttrCache:
    case Opcode::kFloatBinaryOp:
    case Opcode::kFormatValue:
    case Opcode::kFormatWithSpec:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kGetTuple:
    case Opcode::kGuardIs:
    case Opcode::kGuardType:
    case Opcode::kImportFrom:
    case Opcode::kImportName:
    case Opcode::kInPlaceOp:
    case Opcode::kIndexUnbox:
    case Opcode::kInitialYield:
    case Opcode::kIntBinaryOp:
    case Opcode::kInvokeIterNext:
    case Opcode::kInvokeStaticFunction:
    case Opcode::kIsCompactLong:
    case Opcode::kIsInstance:
    case Opcode::kIsNegativeAndErrOccurred:
    case Opcode::kIsTruthy:
    case Opcode::kListAppend:
    case Opcode::kListExtend:
    case Opcode::kListSubscr:
    case Opcode::kLoadArg:
    case Opcode::kLoadArrayItem:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrSpecial:
    case Opcode::kLoadAttrSuper:
    case Opcode::kLoadCellItem:
    case Opcode::kLoadConst:
    case Opcode::kLoadCurrentFunc:
    case Opcode::kLoadEvalBreaker:
    case Opcode::kLoadField:
    case Opcode::kLoadFieldAddress:
    case Opcode::kLoadFunctionIndirect:
    case Opcode::kLoadGlobal:
    case Opcode::kLoadGlobalCached:
    case Opcode::kLoadModuleAttrCached:
    case Opcode::kLoadSplitDictItem:
    case Opcode::kLoadTupleItem:
    case Opcode::kLoadTypeAttrCacheEntryType:
    case Opcode::kLoadTypeAttrCacheEntryValue:
    case Opcode::kLoadTypeMethodCacheEntryType:
    case Opcode::kLoadVarObjectSize:
    case Opcode::kLongBinaryOp:
    case Opcode::kLongCompare:
    case Opcode::kMakeCell:
    case Opcode::kMakeCheckedDict:
    case Opcode::kMakeCheckedList:
    case Opcode::kMakeDict:
    case Opcode::kMakeFunction:
    case Opcode::kMakeList:
    case Opcode::kMakeSet:
    case Opcode::kMakeTuple:
    case Opcode::kMakeTupleFromList:
    case Opcode::kMatchClass:
    case Opcode::kMatchKeys:
    case Opcode::kMaterializeRef:
    case Opcode::kMergeSetUnpack:
    case Opcode::kPhi:
    case Opcode::kPrimitiveBox:
    case Opcode::kPrimitiveBoxBool:
    case Opcode::kPrimitiveCompare:
    case Opcode::kPrimitiveConvert:
    case Opcode::kPrimitiveUnaryOp:
    case Opcode::kPrimitiveUnbox:
    case Opcode::kRefineType:
    case Opcode::kReserveStack:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kStealCellItem:
    case Opcode::kSwapCellItem:
    case Opcode::kTagIfDeferred:
    case Opcode::kTpAlloc:
    case Opcode::kUnaryNot:
    case Opcode::kUnaryOp:
    case Opcode::kUnicodeCompare:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeEqual:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUnpackExToTuple:
    case Opcode::kUnpackSequence:
    case Opcode::kVectorCall:
    case Opcode::kYieldValue:
      return 1;
    case Opcode::kCallInd:
    case Opcode::kFillTypeMethodCache:
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLoadSpecial:
    case Opcode::kLoadTypeMethodCacheEntryValue:
    case Opcode::kSend:
      return 2;
    default:
      break;
  }
  JIT_THROW("Unrecognized opcode {}", hirOpcodeName(op));
}

} // namespace cinderx::jit::hir
