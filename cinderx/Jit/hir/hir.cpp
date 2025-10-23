// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/hir.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/threaded_compile.h"

#include <algorithm>

namespace jit::hir {

// Be intentional about HIR structure sizes.  There's no hard limit on what
// these sizes have to be, but we should be aware when we change them.
static_assert(sizeof(Function) == 48 * kPointerSize);
static_assert(sizeof(CFG) == 6 * kPointerSize);
static_assert(sizeof(BasicBlock) == 21 * kPointerSize);
static_assert(sizeof(Instr) == 6 * kPointerSize);

void DeoptBase::sortLiveRegs() {
  std::sort(
      live_regs_.begin(),
      live_regs_.end(),
      [](const RegState& a, const RegState& b) {
        return a.reg->id() < b.reg->id();
      });

  if (kPyDebug) {
    // Check for uniqueness after sorting rather than inside the predicate
    // passed to std::sort(), in case sort() performs extra comparisons to
    // sanity-check our predicate.
    auto it = std::adjacent_find(
        live_regs_.begin(),
        live_regs_.end(),
        [](const RegState& a, const RegState& b) { return a.reg == b.reg; });
    JIT_DCHECK(it == live_regs_.end(), "Register {} is live twice", *it->reg);
  }
}

std::string_view CallCFunc::funcName() const {
  switch (func_) {
#define FUNC_NAME(V, ...) \
  case Func::k##V:        \
    return #V;
    CallCFunc_FUNCS(FUNC_NAME)
#undef FUNC_NAME
        default : break;
  }
  return "<unknown CallCFunc>";
}

void Phi::setArgs(const std::unordered_map<BasicBlock*, Register*>& args) {
  JIT_DCHECK(NumOperands() == args.size(), "arg mismatch");

  basic_blocks_.clear();
  basic_blocks_.reserve(args.size());

  for (auto& kv : args) {
    basic_blocks_.push_back(kv.first);
  }

  std::sort(
      basic_blocks_.begin(),
      basic_blocks_.end(),
      [](const BasicBlock* a, const BasicBlock* b) -> bool {
        return a->id < b->id;
      });

  std::size_t i = 0;
  for (auto& block : basic_blocks_) {
    operandAt(i) = map_get(args, block);
    i++;
  }
}

std::size_t Phi::blockIndex(const BasicBlock* block) const {
  auto it = std::lower_bound(
      basic_blocks_.begin(), basic_blocks_.end(), block, [](auto b1, auto b2) {
        return b1->id < b2->id;
      });
  JIT_DCHECK(it != basic_blocks_.end(), "Bad CFG");
  JIT_DCHECK(*it == block, "Bad CFG");
  return std::distance(basic_blocks_.begin(), it);
}

OpcodeCounts count_opcodes(const Function& func) {
  OpcodeCounts counts{};
  for (const BasicBlock& block : func.cfg.blocks) {
    for (const Instr& instr : block) {
      counts[static_cast<size_t>(instr.opcode())]++;
    }
  }
  return counts;
}

Edge::~Edge() {
  set_from(nullptr);
  set_to(nullptr);
}

void Edge::set_from(BasicBlock* new_from) {
  if (from_) {
    from_->out_edges_.erase(this);
  }
  if (new_from) {
    new_from->out_edges_.insert(this);
  }
  from_ = new_from;
}

void Edge::set_to(BasicBlock* new_to) {
  if (to_) {
    to_->in_edges_.erase(this);
  }
  if (new_to) {
    new_to->in_edges_.insert(this);
  }
  to_ = new_to;
}

Instr::~Instr() {}

bool Instr::IsTerminator() const {
  switch (opcode()) {
    case Opcode::kBranch:
    case Opcode::kDeopt:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kCondBranchCheckType:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kReturn:
    case Opcode::kUnreachable:
      return true;
    default:
      return false;
  }
}

bool Instr::isReplayable() const {
  switch (opcode()) {
    case Opcode::kAssign:
    case Opcode::kBitCast:
    case Opcode::kBuildString:
    case Opcode::kCast:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCheckVar:
    case Opcode::kCIntToCBool:
    case Opcode::kDoubleBinaryOp:
    case Opcode::kFloatCompare:
    case Opcode::kFormatValue:
    case Opcode::kFormatWithSpec:
    case Opcode::kGetSecondOutput:
    case Opcode::kGuard:
    case Opcode::kGuardIs:
    case Opcode::kGuardType:
    case Opcode::kHintType:
    case Opcode::kIndexUnbox:
    case Opcode::kIntBinaryOp:
    case Opcode::kIntConvert:
    case Opcode::kIsNegativeAndErrOccurred:
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
    case Opcode::kPrimitiveBox:
    case Opcode::kPrimitiveBoxBool:
    case Opcode::kPrimitiveCompare:
    case Opcode::kPrimitiveUnaryOp:
    case Opcode::kPrimitiveUnbox:
    case Opcode::kRaise:
    case Opcode::kRaiseStatic:
    case Opcode::kRefineType:
    case Opcode::kStealCellItem:
    case Opcode::kUpdatePrevInstr:
    case Opcode::kUnicodeCompare:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUseType:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kWaitHandleLoadWaiter: {
      return true;
    }
    case Opcode::kBatchDecref:
    case Opcode::kBeginInlinedFunction:
    case Opcode::kBinaryOp:
    case Opcode::kBranch:
    case Opcode::kBuildSlice:
    case Opcode::kBuildInterpolation:
    case Opcode::kBuildTemplate:
    case Opcode::kCallCFunc:
    case Opcode::kCallEx:
    case Opcode::kCallInd:
    case Opcode::kCallIntrinsic:
    case Opcode::kCallMethod:
    case Opcode::kCallStatic:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCompare:
    case Opcode::kCompareBool:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kConvertValue:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDecref:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kDictMerge:
    case Opcode::kDictSubscr:
    case Opcode::kDictUpdate:
    case Opcode::kEagerImportName:
    case Opcode::kEndInlinedFunction:
    case Opcode::kFillTypeAttrCache:
    case Opcode::kFillTypeMethodCache:
    case Opcode::kFloatBinaryOp:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kGetTuple:
    case Opcode::kImportName:
    case Opcode::kImportFrom:
    case Opcode::kInPlaceOp:
    case Opcode::kIncref:
    case Opcode::kInitialYield:
    case Opcode::kInitFrameCellVars:
    case Opcode::kInvokeIterNext:
    case Opcode::kInvokeStaticFunction:
    case Opcode::kIsInstance:
    case Opcode::kIsTruthy:
    case Opcode::kListAppend:
    case Opcode::kListExtend:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
    case Opcode::kLoadAttrSpecial:
    case Opcode::kLoadAttrSuper:
    case Opcode::kLoadGlobal:
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadModuleAttrCached:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLoadSpecial:
    case Opcode::kLongBinaryOp:
    case Opcode::kLongInPlaceOp:
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
    case Opcode::kMergeSetUnpack:
    case Opcode::kPhi:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kReturn:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSend:
    case Opcode::kSetCellItem:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kSetFunctionAttr:
    case Opcode::kStoreField:
    case Opcode::kSnapshot:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached:
    case Opcode::kStoreSubscr:
    case Opcode::kTpAlloc:
    case Opcode::kUnaryOp:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnpackExToTuple:
    case Opcode::kUnreachable:
    case Opcode::kVectorCall:
    case Opcode::kWaitHandleRelease:
    case Opcode::kYieldAndYieldFrom:
    case Opcode::kYieldFrom:
    case Opcode::kYieldFromHandleStopAsyncIteration:
    case Opcode::kYieldValue:
    case Opcode::kXDecref:
    case Opcode::kXIncref: {
      return false;
    }
  }
  JIT_ABORT("Bad opcode {}", static_cast<int>(opcode()));
}

void Instr::set_block(BasicBlock* block) {
  block_ = block;
  if (IsTerminator()) {
    for (std::size_t i = 0, n = numEdges(); i < n; ++i) {
      edge(i)->set_from(block);
    }
  }
}

void Instr::link(BasicBlock* block) {
  JIT_CHECK(block_ == nullptr, "Instr is already linked");
  set_block(block);
}

void Instr::unlink() {
  JIT_CHECK(block_ != nullptr, "Instr isn't linked");
  block_node_.Unlink();
  set_block(nullptr);
}

const FrameState* Instr::getDominatingFrameState() const {
  if (block_ == nullptr) {
    return nullptr;
  }
  auto rend = block()->crend();
  auto it = block()->const_reverse_iterator_to(*this);
  for (it++; it != rend; it++) {
    if (it->IsSnapshot()) {
      auto snapshot = static_cast<const Snapshot*>(&*it);
      return snapshot->frameState();
    }
    if (!it->isReplayable()) {
      return nullptr;
    }
  }
  return nullptr;
}

BorrowedRef<PyCodeObject> Instr::code() const {
  const FrameState* fs = getDominatingFrameState();
  return fs == nullptr ? block()->cfg->func->code : fs->code;
}

bool isLoadMethodBase(const Instr& instr) {
  return dynamic_cast<const LoadMethodBase*>(&instr) != nullptr;
}

bool isAnyLoadMethod(const Instr& instr) {
  if (isLoadMethodBase(instr)) {
    return true;
  }
  if (!instr.IsPhi() || instr.NumOperands() != 2) {
    return false;
  }
  const Instr* arg1 = instr.GetOperand(0)->instr();
  const Instr* arg2 = instr.GetOperand(1)->instr();
  return (arg1->IsLoadTypeMethodCacheEntryValue() &&
          arg2->IsFillTypeMethodCache()) ||
      (arg2->IsLoadTypeMethodCacheEntryValue() &&
       arg1->IsFillTypeMethodCache());
}

bool isPassthrough(const Instr& instr) {
  switch (instr.opcode()) {
    case Opcode::kAssign:
    case Opcode::kBitCast:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckVar:
    case Opcode::kGuardIs:
    case Opcode::kGuardType:
    case Opcode::kRefineType:
    case Opcode::kUseType:
      return true;

    // Cast is pass-through except when we are casting to float, in which case
    // we may coerce an incoming int to a new float.
    case Opcode::kCast:
      return (static_cast<const Cast*>(&instr))->pytype() != &PyFloat_Type;

    case Opcode::kBinaryOp:
    case Opcode::kBuildSlice:
    case Opcode::kBuildString:
    case Opcode::kBuildInterpolation:
    case Opcode::kBuildTemplate:
    case Opcode::kCallCFunc:
    case Opcode::kCallEx:
    case Opcode::kCallInd:
    case Opcode::kCallIntrinsic:
    case Opcode::kCallMethod:
    case Opcode::kCallStatic:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCIntToCBool:
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
    case Opcode::kFillTypeMethodCache:
    case Opcode::kFloatBinaryOp:
    case Opcode::kFloatCompare:
    case Opcode::kFormatValue:
    case Opcode::kFormatWithSpec:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kGetSecondOutput:
    case Opcode::kGetTuple:
    case Opcode::kImportFrom:
    case Opcode::kImportName:
    case Opcode::kInPlaceOp:
    case Opcode::kIndexUnbox:
    case Opcode::kInitialYield:
    case Opcode::kIntBinaryOp:
    case Opcode::kIntConvert:
    case Opcode::kInvokeIterNext:
    case Opcode::kInvokeStaticFunction:
    case Opcode::kIsInstance:
    case Opcode::kIsNegativeAndErrOccurred:
    case Opcode::kIsTruthy:
    case Opcode::kListAppend:
    case Opcode::kListExtend:
    case Opcode::kLoadArg:
    case Opcode::kLoadArrayItem:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
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
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLoadSpecial:
    case Opcode::kLoadModuleAttrCached:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLoadSplitDictItem:
    case Opcode::kLoadTupleItem:
    case Opcode::kLoadTypeAttrCacheEntryType:
    case Opcode::kLoadTypeAttrCacheEntryValue:
    case Opcode::kLoadTypeMethodCacheEntryType:
    case Opcode::kLoadTypeMethodCacheEntryValue:
    case Opcode::kLoadVarObjectSize:
    case Opcode::kLongBinaryOp:
    case Opcode::kLongInPlaceOp:
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
    case Opcode::kMergeSetUnpack:
    case Opcode::kPhi:
    case Opcode::kPrimitiveBox:
    case Opcode::kPrimitiveBoxBool:
    case Opcode::kPrimitiveCompare:
    case Opcode::kPrimitiveUnaryOp:
    case Opcode::kPrimitiveUnbox:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSend:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kStealCellItem:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached:
    case Opcode::kStoreSubscr:
    case Opcode::kTpAlloc:
    case Opcode::kUnaryOp:
    case Opcode::kUnicodeCompare:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUnpackExToTuple:
    case Opcode::kVectorCall:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kWaitHandleLoadWaiter:
    case Opcode::kYieldAndYieldFrom:
    case Opcode::kYieldFrom:
    case Opcode::kYieldFromHandleStopAsyncIteration:
    case Opcode::kYieldValue:
      return false;

    case Opcode::kBatchDecref:
    case Opcode::kBeginInlinedFunction:
    case Opcode::kBranch:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kDecref:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kEndInlinedFunction:
    case Opcode::kGuard:
    case Opcode::kHintType:
    case Opcode::kIncref:
    case Opcode::kInitFrameCellVars:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kReturn:
    case Opcode::kSetCellItem:
    case Opcode::kSetFunctionAttr:
    case Opcode::kSnapshot:
    case Opcode::kStoreField:
    case Opcode::kUpdatePrevInstr:
    case Opcode::kUnreachable:
    case Opcode::kWaitHandleRelease:
    case Opcode::kXDecref:
    case Opcode::kXIncref:
      JIT_ABORT("Opcode {} has no output", instr.opname());
  }
  JIT_ABORT("Bad opcode {}", static_cast<int>(instr.opcode()));
}

Register* modelReg(Register* reg) {
  auto orig_reg = reg;
  // Even though GuardIs is a passthrough, it verifies that a runtime value is a
  // specific object, breaking the dependency on the instruction that produced
  // the runtime value
  while (isPassthrough(*reg->instr()) && !(reg->instr()->IsGuardIs())) {
    reg = reg->instr()->GetOperand(0);
    JIT_DCHECK(reg != orig_reg, "Hit cycle while looking for model reg");
  }
  return reg;
}

Instr* BasicBlock::Append(Instr* instr) {
  instrs_.PushBack(*instr);
  instr->link(this);
  return instr;
}

void BasicBlock::push_front(Instr* instr) {
  instrs_.PushFront(*instr);
  instr->link(this);
}

Instr* BasicBlock::pop_front() {
  Instr* result = &(instrs_.ExtractFront());
  result->set_block(nullptr);
  return result;
}

void BasicBlock::insert(Instr* instr, Instr::List::iterator it) {
  // If the instruction doesn't come with a bytecode offset, try to take one
  // from an adjacent instruction.
  if (instr->bytecodeOffset() == -1) {
    if (it != instrs_.begin()) {
      instr->setBytecodeOffset(std::prev(it)->bytecodeOffset());
    } else if (it != instrs_.end()) {
      instr->setBytecodeOffset(it->bytecodeOffset());
    }
  }

  instrs_.insert(*instr, it);
  instr->link(this);
}

void BasicBlock::clear() {
  while (!instrs_.IsEmpty()) {
    Instr* instr = &(instrs_.ExtractFront());
    delete instr;
  }
}

BasicBlock::~BasicBlock() {
  JIT_DCHECK(
      in_edges_.empty(), "Attempt to destroy a block with in-edges, {}", id);
  clear();
  JIT_DCHECK(
      out_edges_.empty(), "out_edges not empty after deleting all instrs");
}

Instr* BasicBlock::GetTerminator() {
  if (instrs_.IsEmpty()) {
    return nullptr;
  }
  return &instrs_.Back();
}

Snapshot* BasicBlock::entrySnapshot() {
  for (auto& instr : instrs_) {
    if (instr.IsPhi()) {
      continue;
    }
    if (instr.IsSnapshot()) {
      return static_cast<Snapshot*>(&instr);
    }
    return nullptr;
  }
  return nullptr;
}

bool BasicBlock::IsTrampoline() {
  for (auto& instr : instrs_) {
    if (instr.IsBranch()) {
      auto succ = instr.successor(0);
      // Don't consider a block a trampoline if its successor has one or more
      // Phis, since this block may be necessary to pass a specific value to
      // the Phi. This is correct but conservative: it's often safe to
      // eliminate trampolines that jump to Phis, but that requires more
      // involved analysis in the caller.
      return succ != this && (succ->empty() || !succ->front().IsPhi());
    }
    if (instr.IsSnapshot()) {
      continue;
    }
    return false;
  }
  // empty block
  return false;
}

BasicBlock* BasicBlock::splitAfter(Instr& instr) {
  JIT_CHECK(cfg != nullptr, "cannot split unlinked block");
  auto tail = cfg->AllocateBlock();
  for (auto it = std::next(instrs_.iterator_to(instr)); it != instrs_.end();) {
    auto& instr_2 = *it;
    ++it;
    instr_2.unlink();
    tail->Append(&instr_2);
  }

  for (auto edge : tail->out_edges()) {
    edge->to()->fixupPhis(this, tail);
  }
  return tail;
}

void BasicBlock::fixupPhis(BasicBlock* old_pred, BasicBlock* new_pred) {
  // This won't work correctly if this block has two incoming edges from the
  // same block, but we already can't handle that correctly with our current Phi
  // setup.

  forEachPhi([&](Phi& phi) {
    std::unordered_map<BasicBlock*, Register*> args;
    for (size_t i = 0, n = phi.NumOperands(); i < n; ++i) {
      auto block = phi.basic_blocks()[i];
      if (block == old_pred) {
        block = new_pred;
      }
      args[block] = phi.GetOperand(i);
    }
    phi.setArgs(args);
  });
}

void BasicBlock::addPhiPredecessor(BasicBlock* old_pred, BasicBlock* new_pred) {
  std::vector<Phi*> replacements;
  forEachPhi([&](Phi& phi) {
    for (auto block : phi.basic_blocks()) {
      if (block == old_pred) {
        replacements.push_back(&phi);
        break;
      }
    }
  });

  for (auto phi : replacements) {
    std::unordered_map<BasicBlock*, Register*> args;
    for (size_t i = 0, n = phi->NumOperands(); i < n; ++i) {
      auto block = phi->basic_blocks()[i];
      if (block == old_pred) {
        args[new_pred] = phi->GetOperand(i);
      }
      args[block] = phi->GetOperand(i);
    }

    phi->ReplaceWith(*Phi::create(phi->output(), args));
    delete phi;
  }
}

void BasicBlock::removePhiPredecessor(BasicBlock* old_pred) {
  for (auto it = instrs_.begin(); it != instrs_.end();) {
    auto& instr = *it;
    ++it;
    if (!instr.IsPhi()) {
      break;
    }

    Phi* phi = static_cast<Phi*>(&instr);
    std::unordered_map<BasicBlock*, Register*> args;
    for (size_t i = 0, n = phi->NumOperands(); i < n; ++i) {
      auto block = phi->basic_blocks()[i];
      if (block == old_pred) {
        continue;
      }
      args[block] = phi->GetOperand(i);
    }
    phi->ReplaceWith(*Phi::create(phi->output(), args));
    delete phi;
  }
}

BasicBlock* CFG::AllocateBlock() {
  auto block = AllocateUnlinkedBlock();
  block->cfg = this;
  blocks.PushBack(*block);
  return block;
}

BasicBlock* CFG::AllocateUnlinkedBlock() {
  int id = next_block_id_;
  auto block = new BasicBlock(id);
  next_block_id_++;
  return block;
}

void CFG::InsertBlock(BasicBlock* block) {
  block->cfg = this;
  blocks.PushBack(*block);
}

void CFG::RemoveBlock(BasicBlock* block) {
  JIT_DCHECK(block->cfg == this, "block doesn't belong to us");
  block->cfg_node.Unlink();
  block->cfg = nullptr;
}

void CFG::splitCriticalEdges() {
  std::vector<Edge*> critical_edges;

  // Separately enumerate and process the critical edges to avoid mutating the
  // CFG while iterating it.
  for (auto& block : blocks) {
    auto term = block.GetTerminator();
    JIT_DCHECK(term != nullptr, "Invalid block");
    auto num_edges = term->numEdges();
    if (num_edges < 2) {
      continue;
    }
    for (std::size_t i = 0; i < num_edges; ++i) {
      auto edge = term->edge(i);
      if (edge->to()->in_edges().size() > 1) {
        critical_edges.emplace_back(edge);
      }
    }
  }

  for (auto edge : critical_edges) {
    auto from = edge->from();
    auto to = edge->to();
    auto split_bb = AllocateBlock();
    auto term = edge->from()->GetTerminator();
    split_bb->appendWithOff<Branch>(term->bytecodeOffset(), to);
    edge->set_to(split_bb);
    to->fixupPhis(from, split_bb);
  }
}

static void postorder_traverse(
    BasicBlock* block,
    std::vector<BasicBlock*>* traversal,
    std::unordered_set<BasicBlock*>* visited) {
  JIT_CHECK(block != nullptr, "visiting null block!");
  visited->emplace(block);

  // Add successors to be visited
  Instr* instr = block->GetTerminator();
  switch (instr->opcode()) {
    case Opcode::kCondBranch:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kCondBranchCheckType: {
      auto cbr = static_cast<CondBranch*>(instr);
      if (!visited->contains(cbr->false_bb())) {
        postorder_traverse(cbr->false_bb(), traversal, visited);
      }
      if (!visited->contains(cbr->true_bb())) {
        postorder_traverse(cbr->true_bb(), traversal, visited);
      }
      break;
    }
    case Opcode::kBranch: {
      auto br = static_cast<Branch*>(instr);
      if (!visited->contains(br->target())) {
        postorder_traverse(br->target(), traversal, visited);
      }
      break;
    }
    case Opcode::kDeopt:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kUnreachable:
    case Opcode::kReturn: {
      // No successor blocks
      break;
    }
    default: {
      /* NOTREACHED */
      JIT_ABORT(
          "Block {} has invalid terminator {}", block->id, instr->opname());
    }
  }

  traversal->emplace_back(block);
}

std::vector<BasicBlock*> CFG::GetRPOTraversal() const {
  return GetRPOTraversal(entry_block);
}

std::vector<BasicBlock*> CFG::GetRPOTraversal(BasicBlock* start) {
  auto traversal = GetPostOrderTraversal(start);
  std::reverse(traversal.begin(), traversal.end());
  return traversal;
}

std::vector<BasicBlock*> CFG::GetPostOrderTraversal() const {
  return GetPostOrderTraversal(entry_block);
}

std::vector<BasicBlock*> CFG::GetPostOrderTraversal(BasicBlock* start) {
  std::vector<BasicBlock*> traversal;
  if (start == nullptr) {
    return traversal;
  }
  std::unordered_set<BasicBlock*> visited;
  postorder_traverse(start, &traversal, &visited);
  return traversal;
}

const BasicBlock* CFG::getBlockById(int id) const {
  for (auto& block : blocks) {
    if (block.id == id) {
      return &block;
    }
  }
  return nullptr;
}

CFG::~CFG() {
  while (!blocks.IsEmpty()) {
    BasicBlock* block = &(blocks.ExtractFront());
    // This is the one situation where it's not a bug to delete a reachable
    // block, since we're deleting everything. Clear block's incoming edges so
    // its destructor doesn't complain.
    for (auto it = block->in_edges().begin(); it != block->in_edges().end();) {
      auto edge = *it;
      ++it;
      const_cast<Edge*>(edge)->set_to(nullptr);
    }
    delete block;
  }
}

constexpr std::array<std::string_view, kNumCompareOps> kCompareOpNames = {
#define OP_STR(NAME) #NAME,
    FOREACH_COMPARE_OP(OP_STR)
#undef OP_STR
};

std::string_view GetCompareOpName(CompareOp op) {
  return kCompareOpNames[static_cast<int>(op)];
}

CompareOp ParseCompareOpName(std::string_view name) {
  for (size_t i = 0; i < kCompareOpNames.size(); ++i) {
    if (name == kCompareOpNames[i]) {
      return static_cast<CompareOp>(i);
    }
  }
  JIT_ABORT("Invalid CompareOp '{}'", name);
}

constexpr std::array<std::string_view, kNumPrimitiveCompareOps>
    kPrimitiveCompareOpNames = {
#define OP_STR(NAME) #NAME,
        FOREACH_PRIMITIVE_COMPARE_OP(OP_STR)
#undef OP_STR
};

std::string_view GetPrimitiveCompareOpName(PrimitiveCompareOp op) {
  return kPrimitiveCompareOpNames[static_cast<int>(op)];
}

PrimitiveCompareOp ParsePrimitiveCompareOpName(std::string_view name) {
  for (size_t i = 0; i < kPrimitiveCompareOpNames.size(); i++) {
    if (name == kPrimitiveCompareOpNames[i]) {
      return static_cast<PrimitiveCompareOp>(i);
    }
  }
  JIT_ABORT("Invalid PrimitiveCompareOp '{}'", name);
}

std::optional<PrimitiveCompareOp> toPrimitiveCompareOp(CompareOp op) {
  switch (op) {
    case CompareOp::kLessThan:
      return PrimitiveCompareOp::kLessThan;
    case CompareOp::kLessThanEqual:
      return PrimitiveCompareOp::kLessThanEqual;
    case CompareOp::kLessThanUnsigned:
      return PrimitiveCompareOp::kLessThanUnsigned;
    case CompareOp::kLessThanEqualUnsigned:
      return PrimitiveCompareOp::kLessThanEqualUnsigned;
    case CompareOp::kEqual:
      return PrimitiveCompareOp::kEqual;
    case CompareOp::kNotEqual:
      return PrimitiveCompareOp::kNotEqual;
    case CompareOp::kGreaterThan:
      return PrimitiveCompareOp::kGreaterThan;
    case CompareOp::kGreaterThanEqual:
      return PrimitiveCompareOp::kGreaterThanEqual;
    case CompareOp::kGreaterThanUnsigned:
      return PrimitiveCompareOp::kGreaterThanUnsigned;
    case CompareOp::kGreaterThanEqualUnsigned:
      return PrimitiveCompareOp::kGreaterThanEqualUnsigned;
    default:
      return std::nullopt;
  }
}

constexpr std::array<std::string_view, kNumBinaryOpKinds> kBinaryOpNames = {
#define OP_STR(NAME) #NAME,
    FOREACH_BINARY_OP_KIND(OP_STR)
#undef OP_STR
};

std::string_view GetBinaryOpName(BinaryOpKind op) {
  return kBinaryOpNames[static_cast<int>(op)];
}

BinaryOpKind ParseBinaryOpName(std::string_view name) {
  for (size_t i = 0; i < kBinaryOpNames.size(); ++i) {
    if (name == kBinaryOpNames[i]) {
      return static_cast<BinaryOpKind>(i);
    }
  }
  JIT_ABORT("Invalid BinaryOpKind '{}'", name);
}

constexpr std::array<std::string_view, kNumUnaryOpKinds> kUnaryOpNames = {
#define OP_STR(NAME) #NAME,
    FOREACH_UNARY_OP_KIND(OP_STR)
#undef OP_STR
};

std::string_view GetUnaryOpName(UnaryOpKind op) {
  return kUnaryOpNames[static_cast<int>(op)];
}

UnaryOpKind ParseUnaryOpName(std::string_view name) {
  for (size_t i = 0; i < kUnaryOpNames.size(); ++i) {
    if (name == kUnaryOpNames[i]) {
      return static_cast<UnaryOpKind>(i);
    }
  }
  JIT_ABORT("Invalid UnaryOpKind '{}'", name);
}

constexpr std::array<std::string_view, kNumPrimitiveUnaryOpKinds>
    kPrimitiveUnaryOpNames = {
#define OP_STR(NAME) #NAME,
        FOREACH_PRIMITIVE_UNARY_OP_KIND(OP_STR)
#undef OP_STR
};

std::string_view GetPrimitiveUnaryOpName(PrimitiveUnaryOpKind op) {
  return kPrimitiveUnaryOpNames[static_cast<int>(op)];
}

PrimitiveUnaryOpKind ParsePrimitiveUnaryOpName(std::string_view name) {
  for (size_t i = 0; i < kPrimitiveUnaryOpNames.size(); ++i) {
    if (name == kPrimitiveUnaryOpNames[i]) {
      return static_cast<PrimitiveUnaryOpKind>(i);
    }
  }
  JIT_ABORT("Invalid PrimitiveUnaryOpKind '{}'", name);
}

// NB: This needs to be in the order that the values appear in the InPlaceOpKind
// enum
constexpr std::array<std::string_view, kNumInPlaceOpKinds> kInPlaceOpNames = {
#define OP_STR(NAME) #NAME,
    FOREACH_INPLACE_OP_KIND(OP_STR)
#undef OP_STR
};

std::string_view GetInPlaceOpName(InPlaceOpKind op) {
  return kInPlaceOpNames[static_cast<int>(op)];
}

InPlaceOpKind ParseInPlaceOpName(std::string_view name) {
  for (size_t i = 0; i < kInPlaceOpNames.size(); ++i) {
    if (name == kInPlaceOpNames[i]) {
      return static_cast<InPlaceOpKind>(i);
    }
  }
  JIT_ABORT("Invalid InPlaceOpKind '{}'", name);
}

// NB: This needs to be in the order that the values appear in the FunctionAttr
// enum
static const char* gFunctionFields[] = {
    "func_closure",
    "func_annotations",
    "func_kwdefaults",
    "func_defaults",
};

const char* functionFieldName(FunctionAttr field) {
  return gFunctionFields[static_cast<int>(field)];
}

Environment::~Environment() {
  // Serialize as we modify the ref-count of objects which may be widely
  // accessible.
  ThreadedCompileSerialize guard;
  references_.clear();
}

Register* Environment::AllocateRegister() {
  auto id = next_register_id_++;
  while (registers_.contains(id)) {
    id = next_register_id_++;
  }
  auto res = registers_.emplace(id, std::make_unique<Register>(id));
  return res.first->second.get();
}

Register* Environment::getRegister(int id) {
  auto it = registers_.find(id);
  if (it == registers_.end()) {
    return nullptr;
  }
  return it->second.get();
}

const Environment::RegisterMap& Environment::GetRegisters() const {
  return registers_;
}

Function::Function() {
  cfg.func = this;
}

Function::~Function() {
  // Serialize as we alter ref-counts on potentially global objects.
  ThreadedCompileSerialize guard;
  code.reset();
  builtins.reset();
  globals.reset();
  prim_args_info.reset();
}

Register* Environment::addRegister(std::unique_ptr<Register> reg) {
  auto id = reg->id();
  auto res = registers_.emplace(id, std::move(reg));
  JIT_CHECK(res.second, "Register {} already in map", id);
  return res.first->second.get();
}

BorrowedRef<> Environment::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  return references_.emplace(ThreadedRef<>::create(obj)).first->get();
}

const Environment::ReferenceSet& Environment::references() const {
  return references_;
}

bool usesRuntimeFunc(BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX < 0x030C0000
  return PyTuple_GET_SIZE(PyCode_GetFreevars(code)) > 0;
#else
  // In 3.12+ we always need the runtime function because we use it to
  // initialize the _PyInterpreterFrame object.
  return true;
#endif
}

void Function::setCode(BorrowedRef<PyCodeObject> code_2) {
  this->code.reset(code_2);
  uses_runtime_func = usesRuntimeFunc(code_2);
  frameMode = getConfig().frame_mode;
}

std::size_t Function::CountInstrs(InstrPredicate pred) const {
  std::size_t result = 0;
  for (const auto& block : cfg.blocks) {
    for (const auto& instr : block) {
      if (pred(instr)) {
        result++;
      }
    }
  }
  return result;
}

int Function::numArgs() const {
  if (code == nullptr) {
    // code might be null if we parsed from textual ir
    return 0;
  }
  return code->co_argcount + code->co_kwonlyargcount +
      bool(code->co_flags & CO_VARARGS) + bool(code->co_flags & CO_VARKEYWORDS);
}

Py_ssize_t Function::numVars() const {
  // Code might be null if we parsed from textual HIR.
  return code != nullptr ? numLocalsplus(code) : 0;
}

bool Function::canDeopt() const {
  for (const BasicBlock& block : cfg.blocks) {
    for (const Instr& instr : block) {
      if (instr.asDeoptBase()) {
        return true;
      }
    }
  }
  return false;
}

const char* const kFailureTypeMsgs[] = {
#define FAILURE_TYPE_MSG(failure, msg) msg,
    FOREACH_FAILURE_TYPE(FAILURE_TYPE_MSG)
#undef NAME_FAILURE_TYPE
};

const char* const kFailureTypeNames[] = {
#define NAME_TYPE(failure, msg) #failure,
    FOREACH_FAILURE_TYPE(NAME_TYPE)
#undef NAME_TYPE
};

const char* getInlineFailureMessage(InlineFailureType failure_type) {
  return kFailureTypeMsgs[static_cast<size_t>(failure_type)];
}

const char* getInlineFailureName(InlineFailureType failure_type) {
  return kFailureTypeNames[static_cast<size_t>(failure_type)];
}

std::ostream& operator<<(std::ostream& os, OperandType op) {
  switch (op.kind) {
    case Constraint::kType:
      return os << op.type;
    case Constraint::kOptObjectOrCIntOrCBool:
      return os << "(OptObject, CInt, CBool)";
    case Constraint::kOptObjectOrCInt:
      return os << "(OptObject, CInt)";
    case Constraint::kTupleExactOrCPtr:
      return os << "(TupleExact, CPtr)";
    case Constraint::kListOrChkList:
      return os << "(List, chklist)";
    case Constraint::kDictOrChkDict:
      return os << "(Dict, chkdict)";
    case Constraint::kMatchAllAsCInt:
      return os << "CInt";
    case Constraint::kMatchAllAsPrimitive:
      return os << "Primitive";
  }
  JIT_ABORT("unknown constraint");
  return os << "<unknown>";
}

const FrameState* get_frame_state(const Instr& instr) {
  if (instr.IsSnapshot()) {
    return static_cast<const Snapshot&>(instr).frameState();
  }
  if (instr.IsBeginInlinedFunction()) {
    return static_cast<const BeginInlinedFunction&>(instr).callerFrameState();
  }
  if (auto db = instr.asDeoptBase()) {
    return db->frameState();
  }
  return nullptr;
}

FrameState* get_frame_state(Instr& instr) {
  return const_cast<FrameState*>(
      get_frame_state(const_cast<const Instr&>(instr)));
}

} // namespace jit::hir
