// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/instr_effects.h"

#include "cinderx/Jit/hir/alias_class.h"
#include "cinderx/Jit/hir/hir.h"

namespace jit::hir {

namespace {
// Instructions that don't produce a borrowed reference or steal any of their
// inputs.
MemoryEffects commonEffects(const Instr& inst, AliasClass may_store) {
  return {false, AEmpty, {inst.NumOperands()}, may_store};
}

// Instructions that borrow their output from a specific location.
MemoryEffects borrowFrom(const Instr& inst, AliasClass borrow_support) {
  return {true, borrow_support, {inst.NumOperands()}, AEmpty};
}

util::BitVector stealAllInputs(const Instr& inst) {
  return {inst.NumOperands(), (uint64_t{1} << inst.NumOperands()) - 1};
}

} // namespace

MemoryEffects memoryEffects(const Instr& inst) {
  switch (inst.opcode()) {
    // Instructions that don't produce a borrowed reference, don't steal any
    // inputs, and don't write to heap locations that we track.
    case Opcode::kAssign:
    case Opcode::kBitCast:
    case Opcode::kBuildSlice:
    case Opcode::kBuildString:
    case Opcode::kCast:
    case Opcode::kDeopt:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kDoubleBinaryOp:
    case Opcode::kGetSecondOutput:
    case Opcode::kHintType:
    case Opcode::kIndexUnbox:
    case Opcode::kIntBinaryOp:
    case Opcode::kIntConvert:
    case Opcode::kIsNegativeAndErrOccurred:
    case Opcode::kLoadEvalBreaker:
    case Opcode::kLoadVarObjectSize:
    case Opcode::kLongCompare:
    case Opcode::kMakeCell:
    case Opcode::kMakeCheckedDict:
    case Opcode::kMakeDict:
    case Opcode::kMakeSet:
    case Opcode::kMakeTupleFromList:
    case Opcode::kPrimitiveCompare:
    case Opcode::kPrimitiveUnaryOp:
    case Opcode::kPrimitiveUnbox:
    case Opcode::kRefineType:
    case Opcode::kSnapshot:
    case Opcode::kTpAlloc:
    case Opcode::kUnicodeCompare:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUnreachable:
    case Opcode::kUseType:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kWaitHandleLoadWaiter:
      return commonEffects(inst, AEmpty);

    // If boxing a bool, we return a borrowed reference to Py_True or Py_False.
    case Opcode::kPrimitiveBoxBool:
      return borrowFrom(inst, AEmpty);

    case Opcode::kPrimitiveBox:
      return commonEffects(inst, AEmpty);

    // These push/pop shadow frames and should not get DCE'd.
    case Opcode::kBeginInlinedFunction:
    case Opcode::kEndInlinedFunction:
    // Updates the _PyInterpreterFrame
    case Opcode::kUpdatePrevInstr:
    // Can write to fields of its operands.
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kWaitHandleRelease:
      return commonEffects(inst, AOther);

    // These can deopt but don't write to any memory locations when they fall
    // through.
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCheckVar:
    case Opcode::kGuard:
    case Opcode::kGuardType:
      return commonEffects(inst, AEmpty);

    // Instructions that don't produce a borrowed reference, don't steal any
    // inputs, and may write all memory locations (usually from invoking
    // arbitrary user code).
    case Opcode::kBinaryOp:
    case Opcode::kCallEx:
    case Opcode::kCallIntrinsic:
    case Opcode::kCallMethod:
    case Opcode::kCallStatic:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCompare:
    case Opcode::kCompareBool:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteSubscr:
    case Opcode::kDictMerge:
    case Opcode::kDictUpdate:
    case Opcode::kDictSubscr:
    case Opcode::kEagerImportName:
    case Opcode::kFillTypeAttrCache:
    case Opcode::kFillTypeMethodCache:
    case Opcode::kFormatValue:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kImportFrom:
    case Opcode::kImportName:
    case Opcode::kInPlaceOp:
    case Opcode::kInvokeIterNext:
    case Opcode::kInvokeMethod:
    case Opcode::kInvokeMethodStatic:
    case Opcode::kInvokeStaticFunction:
    case Opcode::kIsInstance:
    case Opcode::kIsTruthy:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
    case Opcode::kLoadAttrSpecial:
    case Opcode::kLoadAttrSuper:
    case Opcode::kLoadGlobal:
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLongBinaryOp:
    case Opcode::kLongInPlaceOp:
    case Opcode::kMatchClass:
    case Opcode::kMatchKeys:
    case Opcode::kSend:
    case Opcode::kUnaryOp:
    case Opcode::kUnpackExToTuple:
    case Opcode::kVectorCall:
      return commonEffects(inst, AManagedHeapAny);

    // Steals the reference to its second input and gives it to the cell
    case Opcode::kSetCellItem:
      return {true, AEmpty, {inst.NumOperands(), 2}, ACellItem};

    // Returns a stolen (from the cell), not borrowed, reference.
    case Opcode::kStealCellItem:
      return commonEffects(inst, AEmpty);

    // Instructions that return nullptr or a borrowed reference to a singleton
    // (usually None or True), and can invoke user code.
    case Opcode::kMergeSetUnpack:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached:
    case Opcode::kStoreSubscr:
      return {true, AEmpty, {}, AManagedHeapAny};

    case Opcode::kListAppend:
    case Opcode::kListExtend:
      return {true, AEmpty, {inst.NumOperands()}, AListItem};

    case Opcode::kIncref:
    case Opcode::kXIncref:
      return {false, AEmpty, {inst.NumOperands()}, AOther};

    case Opcode::kBatchDecref:
    case Opcode::kDecref:
    case Opcode::kXDecref:
      return {false, AEmpty, {1, 1}, AManagedHeapAny};

    case Opcode::kMakeFunction:
      // MakeFunction can invoke the JIT which may at some point have effects
      // worth tracking.
      return commonEffects(inst, AOther);

    case Opcode::kMakeCheckedList:
    case Opcode::kMakeList:
    case Opcode::kMakeTuple: {
      // Steal all inputs.
      util::BitVector inputs{inst.NumOperands()};
      inputs.fill(true);
      auto may_store =
          inst.opcode() == Opcode::kMakeTuple ? ATupleItem : AListItem;
      return {false, AEmpty, std::move(inputs), may_store};
    }

    case Opcode::kStoreField:
      JIT_DCHECK(inst.NumOperands() == 3, "Unexpected number of operands");
      return {false, AEmpty, {3, 2}, AInObjectAttr};

    case Opcode::kLoadArg:
    case Opcode::kLoadCurrentFunc:
      return borrowFrom(inst, AFuncArgs);

    case Opcode::kGuardIs:
    case Opcode::kLoadConst:
      return borrowFrom(inst, AEmpty);

    case Opcode::kLoadCellItem:
      return borrowFrom(inst, ACellItem);

    case Opcode::kLoadField: {
      auto& ldfld = static_cast<const LoadField&>(inst);
      if (ldfld.borrowed()) {
        return borrowFrom(inst, AInObjectAttr);
      }
      return commonEffects(inst, AEmpty);
    }

    case Opcode::kLoadFieldAddress:
      return commonEffects(inst, AEmpty);

    case Opcode::kLoadFunctionIndirect:

    case Opcode::kLoadGlobalCached:
      return borrowFrom(inst, AGlobal);

    case Opcode::kLoadTupleItem:
      return borrowFrom(inst, ATupleItem);

    case Opcode::kLoadArrayItem:
      return borrowFrom(inst, AArrayItem | AListItem);
    case Opcode::kStoreArrayItem:
      // we steal a ref to our third operand, the value being stored
      return {
          false, AEmpty, {inst.NumOperands(), 1 << 2}, AArrayItem | AListItem};
    case Opcode::kLoadSplitDictItem:
      return borrowFrom(inst, ADictItem);
    case Opcode::kLoadTypeAttrCacheEntryType:
    case Opcode::kLoadTypeAttrCacheEntryValue:
      return borrowFrom(inst, ATypeAttrCache);
    case Opcode::kLoadTypeMethodCacheEntryValue:
      // This instruction will return a struct containing 2 pointers where the
      // second pointer is emitted as an output by GetLoadMethodInstance who
      // does not produce a borrowed reference. We are choosing to also not
      // produce borrowed reference here to be consistent with
      // GetLoadMethodInstance's memory effects for simplicity
      return commonEffects(inst, AEmpty);
    case Opcode::kLoadTypeMethodCacheEntryType:
      return borrowFrom(inst, ATypeMethodCache);

    case Opcode::kReturn:
      return {false, AEmpty, {1, 1}, AManagedHeapAny};

    case Opcode::kSetFunctionAttr: {
      JIT_DCHECK(inst.NumOperands() == 2, "Unexpected number of operands");
      return {false, AEmpty, {2, 1}, AFuncAttr};
    }

    case Opcode::kRaise:
      return {false, AEmpty, stealAllInputs(inst), AEmpty};

    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
      return commonEffects(inst, AManagedHeapAny);

    // The outputs of InitialYield and YieldValue are the `arg` argument to
    // _PyJIT_GenSend(), which is borrowed from its caller like all arguments
    // to C functions.
    case Opcode::kInitialYield:
      return {true, AFuncArgs, {inst.NumOperands()}, AAny};
    case Opcode::kYieldValue:
      return {true, AFuncArgs, {1, 1}, AAny};

    case Opcode::kYieldFrom:
#if PY_VERSION_HEX >= 0x030C0000
      // In 3.12+ YieldFrom is actually YieldValue but has an additional arg for
      // the subiterator for use when querying yield-from.
      return {true, AFuncArgs, {2, 1}, AAny};
#else
      [[fallthrough]];
#endif
    case Opcode::kYieldFromHandleStopAsyncIteration: {
      // In 3.10 YieldFrom's output is either the yielded value from the subiter
      // or the final result from a StopIteration, and is owned in either case.
      return commonEffects(inst, AAny);
    }
    // YieldAndYieldFrom is equivalent to YieldFrom composed with YieldValue,
    // and steals the value it yields to the caller.
    case Opcode::kYieldAndYieldFrom: {
      return {false, AEmpty, {2, 1}, AAny};
    }

    case Opcode::kCallCFunc:
      return commonEffects(inst, AManagedHeapAny);

    case Opcode::kBranch:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kPhi:
      JIT_ABORT(
          "Opcode {} doesn't have well-defined memory effects", inst.opname());
    case Opcode::kGetTuple:
      return commonEffects(inst, AAny);
  }

  JIT_ABORT("Bad opcode {}", static_cast<int>(inst.opcode()));
}

bool hasArbitraryExecution(const Instr& inst) {
  switch (inst.opcode()) {
    /*
     * No arbitrary execution.
     */

    // Deopting opcodes which don't have side-effects otherwise. Assume getting
    // us into and resuming execution in the interpreter is sufficient to not
    // need special handling for effective side-effects.
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCheckVar:
    case Opcode::kDeopt:
    case Opcode::kGuard:
    case Opcode::kGuardType:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:

    // Counting return as NOT arbitrary for now. Assume returning from the
    // function is sufficient to not need special handling for further effects.
    case Opcode::kReturn:

    case Opcode::kAssign:
    case Opcode::kBeginInlinedFunction:
    case Opcode::kBitCast:
    case Opcode::kBranch:
    case Opcode::kBuildSlice:
    case Opcode::kBuildString:
    case Opcode::kCast:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kDoubleBinaryOp:
    case Opcode::kEndInlinedFunction:
    case Opcode::kGetSecondOutput:
    case Opcode::kGuardIs:
    case Opcode::kHintType:
    case Opcode::kIncref:
    case Opcode::kIndexUnbox:
    case Opcode::kIntBinaryOp:
    case Opcode::kIntConvert:
    case Opcode::kIsNegativeAndErrOccurred:
    case Opcode::kListAppend:
    case Opcode::kListExtend:
    case Opcode::kLoadArg:
    case Opcode::kLoadArrayItem:
    case Opcode::kLoadCellItem:
    case Opcode::kLoadConst:
    case Opcode::kLoadCurrentFunc:
    case Opcode::kLoadEvalBreaker:
    case Opcode::kLoadField:
    case Opcode::kLoadFieldAddress:
    case Opcode::kLoadFunctionIndirect:
    case Opcode::kLoadGlobalCached:
    case Opcode::kLoadSplitDictItem:
    case Opcode::kLoadTupleItem:
    case Opcode::kLoadTypeAttrCacheEntryType:
    case Opcode::kLoadTypeAttrCacheEntryValue:
    case Opcode::kLoadTypeMethodCacheEntryType:
    case Opcode::kLoadTypeMethodCacheEntryValue:
    case Opcode::kLoadVarObjectSize:
    case Opcode::kLongCompare:
    case Opcode::kMakeCell:
    case Opcode::kMakeCheckedDict:
    case Opcode::kMakeCheckedList:
    case Opcode::kMakeDict:
    case Opcode::kMakeList:
    case Opcode::kMakeSet:
    case Opcode::kMakeTuple:
    case Opcode::kMakeTupleFromList:
    case Opcode::kPhi:
    case Opcode::kPrimitiveBox:
    case Opcode::kPrimitiveBoxBool:
    case Opcode::kPrimitiveCompare:
    case Opcode::kPrimitiveUnaryOp:
    case Opcode::kPrimitiveUnbox:
    case Opcode::kRefineType:
    case Opcode::kSetCellItem:
    case Opcode::kSetFunctionAttr:
    case Opcode::kSnapshot:
    case Opcode::kStealCellItem:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreField:
    case Opcode::kTpAlloc:
    case Opcode::kUnicodeCompare:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUnreachable:
    case Opcode::kUpdatePrevInstr:
    case Opcode::kUseType:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kWaitHandleLoadWaiter:
    case Opcode::kWaitHandleRelease:
    case Opcode::kXIncref:
      return false;

    /*
     * Opcodes which do have potential arbitrary execution.
     */
    case Opcode::kBatchDecref:
    case Opcode::kBinaryOp:
    case Opcode::kCallEx:
    case Opcode::kCallIntrinsic:
    case Opcode::kCallMethod:
    case Opcode::kCallStatic:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCompare:
    case Opcode::kCompareBool:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDecref:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteSubscr:
    case Opcode::kDictMerge:
    case Opcode::kDictSubscr:
    case Opcode::kDictUpdate:
    case Opcode::kEagerImportName:
    case Opcode::kFillTypeAttrCache:
    case Opcode::kFillTypeMethodCache:
    case Opcode::kFormatValue:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kGetTuple:
    case Opcode::kImportFrom:
    case Opcode::kImportName:
    case Opcode::kInitialYield:
    case Opcode::kInPlaceOp:
    case Opcode::kInvokeIterNext:
    case Opcode::kInvokeMethod:
    case Opcode::kInvokeMethodStatic:
    case Opcode::kInvokeStaticFunction:
    case Opcode::kIsInstance:
    case Opcode::kIsTruthy:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
    case Opcode::kLoadAttrSpecial:
    case Opcode::kLoadAttrSuper:
    case Opcode::kLoadGlobal:
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLongBinaryOp:
    case Opcode::kLongInPlaceOp:
    case Opcode::kMakeFunction:
    case Opcode::kMergeSetUnpack:
    case Opcode::kMatchClass:
    case Opcode::kMatchKeys:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSend:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached:
    case Opcode::kStoreSubscr:
    case Opcode::kUnaryOp:
    case Opcode::kUnpackExToTuple:
    case Opcode::kVectorCall:
    case Opcode::kXDecref:
    case Opcode::kYieldAndYieldFrom:
    case Opcode::kYieldFrom:
    case Opcode::kYieldFromHandleStopAsyncIteration:
    case Opcode::kYieldValue:
      return true;

    case Opcode::kCallCFunc:
      switch (static_cast<const CallCFunc&>(inst).func()) {
#if PY_VERSION_HEX >= 0x030C0000
        case CallCFunc::Func::kJitCoro_GetAwaitableIter:
          return true;
        case CallCFunc::Func::kCix_PyAsyncGenValueWrapperNew:
          return false;
        case CallCFunc::Func::kJitGen_yf:
          return false;
#else
        case CallCFunc::Func::kCix_PyCoro_GetAwaitableIter:
          return true;
        case CallCFunc::Func::kCix_PyAsyncGenValueWrapperNew:
          return false;
        case CallCFunc::Func::kCix_PyGen_yf:
          return false;
#endif
      }
      JIT_ABORT(
          "Bad CallCFunc function {}",
          static_cast<const CallCFunc&>(inst).func());
  }

  JIT_ABORT("Bad opcode {}", static_cast<int>(inst.opcode()));
}

} // namespace jit::hir
