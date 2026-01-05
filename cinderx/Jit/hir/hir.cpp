// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/hir.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/threaded_compile.h"

#include <algorithm>

namespace jit::hir {

DeoptBase::DeoptBase(Opcode op) : Instr(op) {}

DeoptBase::DeoptBase(Opcode op, const FrameState& frame) : Instr(op) {
  setFrameState(frame);
}

DeoptBase::DeoptBase(const DeoptBase& other)
    : Instr(other),
      live_regs_{other.live_regs()},
      guilty_reg_{other.guiltyReg()},
      nonce_{other.nonce()},
      descr_{other.descr()} {
  if (FrameState* copy_fs = other.frameState()) {
    setFrameState(std::make_unique<FrameState>(*copy_fs));
  }
}

const std::vector<RegState>& DeoptBase::live_regs() const {
  return live_regs_;
}

std::vector<RegState>& DeoptBase::live_regs() {
  return live_regs_;
}

DeoptBase* DeoptBase::asDeoptBase() {
  return this;
}

const DeoptBase* DeoptBase::asDeoptBase() const {
  return this;
}

bool DeoptBase::visitUses(const std::function<bool(Register*&)>& func) {
  if (!Instr::visitUses(func)) {
    return false;
  }
  if (auto fs = frameState()) {
    if (!fs->visitUses(func)) {
      return false;
    }
  }
  for (auto& reg_state : live_regs_) {
    if (!func(reg_state.reg)) {
      return false;
    }
  }
  if (guilty_reg_ != nullptr) {
    if (!func(guilty_reg_)) {
      return false;
    }
  }
  return true;
}

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

void DeoptBase::setFrameState(std::unique_ptr<FrameState> state) {
  frame_state_ = std::move(state);
}

void DeoptBase::setFrameState(const FrameState& state) {
  frame_state_ = std::make_unique<FrameState>(state);
}

FrameState* DeoptBase::frameState() const {
  return frame_state_.get();
}

std::unique_ptr<FrameState> DeoptBase::takeFrameState() {
  return std::move(frame_state_);
}

int DeoptBase::nonce() const {
  return nonce_;
}

void DeoptBase::set_nonce(int nonce) {
  nonce_ = nonce;
}

const std::string& DeoptBase::descr() const {
  return descr_;
}

void DeoptBase::setDescr(std::string r) {
  descr_ = std::move(r);
}

Register* DeoptBase::guiltyReg() const {
  return guilty_reg_;
}

void DeoptBase::setGuiltyReg(Register* reg) {
  guilty_reg_ = reg;
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

Edge::Edge(const Edge& other) {
  set_from(other.from_);
  set_to(other.to_);
}

Edge::~Edge() {
  set_from(nullptr);
  set_to(nullptr);
}

BasicBlock* Edge::from() const {
  return from_;
}

BasicBlock* Edge::to() const {
  return to_;
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

void* Instr::allocate(std::size_t fixed_size, std::size_t num_operands) {
  auto variable_size = num_operands * kPointerSize;
  char* ptr = static_cast<char*>(
      calloc(variable_size + fixed_size + sizeof(std::size_t), 1));
  ptr += variable_size;
  *reinterpret_cast<size_t*>(ptr) = num_operands;
  ptr += sizeof(std::size_t);
  return ptr;
}

void* Instr::operator new(std::size_t count, void* ptr) {
  return ::operator new(count, ptr);
}

void Instr::operator delete(void* ptr) {
  auto instr = static_cast<Instr*>(ptr);
  free(instr->base());
}

Instr::Instr(Opcode opcode) : opcode_{opcode} {}

Instr::Instr(const Instr& other)
    : opcode_(other.opcode()),
      bytecode_offset_{other.bytecodeOffset()},
      output_{other.output()} {}

std::string_view Instr::opname() const {
  return hirOpcodeName(opcode());
}

std::size_t Instr::NumOperands() const {
  return *(reinterpret_cast<const std::size_t*>(this) - 1);
}

Register* Instr::GetOperand(std::size_t i) const {
  return const_cast<Instr*>(this)->operandAt(i);
}

std::span<Register* const> Instr::GetOperands() const {
  return {operands(), NumOperands()};
}

void Instr::SetOperand(std::size_t i, Register* reg) {
  operandAt(i) = reg;
}

bool Instr::visitUses(const std::function<bool(Register*&)>& func) {
  auto num_uses = NumOperands();
  for (std::size_t i = 0; i < num_uses; i++) {
    if (!func(operandAt(i))) {
      return false;
    }
  }
  return true;
}

bool Instr::visitUses(const std::function<bool(Register*)>& func) const {
  return const_cast<Instr*>(this)->visitUses(
      [&func](Register*& reg) { return func(reg); });
}

bool Instr::Uses(Register* needle) const {
  bool found = false;
  visitUses([&](const Register* reg) {
    if (reg == needle) {
      found = true;
      return false;
    }
    return true;
  });
  return found;
}

void Instr::ReplaceUsesOf(Register* orig, Register* replacement) {
  visitUses([&](Register*& reg) {
    if (reg == orig) {
      reg = replacement;
    }
    return true;
  });
}

Register* Instr::output() const {
  return output_;
}

void Instr::setOutput(Register* dst) {
  if (output_ != nullptr) {
    output_->set_instr(nullptr);
  }
  if (dst != nullptr) {
    dst->set_instr(this);
  }
  output_ = dst;
}

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

std::size_t Instr::numEdges() const {
  return edges().size();
}

Edge* Instr::edge(std::size_t i) {
  return const_cast<Edge*>(const_cast<const Instr*>(this)->edge(i));
}

const Edge* Instr::edge(std::size_t i) const {
  auto es = edges();
  JIT_CHECK(
      i < es.size(),
      "Trying to access edge {} of {} but it only has {}",
      i,
      opname(),
      es.size());
  return &es[i];
}

std::span<const Edge> Instr::edges() const {
  return {};
}

BasicBlock* Instr::successor(std::size_t i) const {
  return edge(i)->to();
}

void Instr::set_successor(std::size_t i, BasicBlock* to) {
  edge(i)->set_to(to);
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

void Instr::InsertBefore(Instr& instr) {
  block_node_.InsertBefore(&instr.block_node_);
  link(instr.block());
}

void Instr::InsertAfter(Instr& instr) {
  block_node_.InsertAfter(&instr.block_node_);
  link(instr.block());
}

void Instr::ReplaceWith(Instr& instr) {
  instr.InsertBefore(*this);
  instr.setBytecodeOffset(bytecodeOffset());
  unlink();
}

void Instr::ExpandInto(const std::vector<Instr*>& expansion) {
  Instr* last = this;
  for (Instr* instr : expansion) {
    instr->InsertAfter(*last);
    instr->setBytecodeOffset(bytecodeOffset());
    last = instr;
  }
  unlink();
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

BasicBlock* Instr::block() const {
  return block_;
}

BCOffset Instr::bytecodeOffset() const {
  return bytecode_offset_;
}

void Instr::setBytecodeOffset(BCOffset off) {
  bytecode_offset_ = off;
}

void Instr::copyBytecodeOffset(const Instr& instr) {
  setBytecodeOffset(instr.bytecodeOffset());
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

DeoptBase* Instr::asDeoptBase() {
  return nullptr;
}

const DeoptBase* Instr::asDeoptBase() const {
  return nullptr;
}

void* Instr::base() {
  return reinterpret_cast<char*>(this) - (NumOperands() * kPointerSize) -
      sizeof(size_t);
}

Register** Instr::operands() {
  return static_cast<Register**>(base());
}

Register* const* Instr::operands() const {
  return const_cast<Instr*>(this)->operands();
}

Register*& Instr::operandAt(std::size_t i) {
  JIT_DCHECK(
      i < NumOperands(),
      "operand {} out of range (max is {})",
      i,
      NumOperands() - 1);
  return operands()[i];
}

std::span<const Edge> Branch::edges() const {
  return std::span{&edge_, 1};
}

std::span<const Edge> CondBranchBase::edges() const {
  // Depends on the struct layout.  We expect false_edge_ to follow immediately
  // after true_edge_.
  return std::span{&true_edge_, 2};
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

void BasicBlock::retargetPreds(BasicBlock* target) {
  JIT_CHECK(target != this, "Can't retarget to self");
  for (auto it = in_edges_.begin(); it != in_edges_.end();) {
    auto edge = *it;
    ++it;
    const_cast<Edge*>(edge)->set_to(target);
  }
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
    "func_annotate",
};

const char* functionFieldName(FunctionAttr field) {
  return gFunctionFields[static_cast<int>(field)];
}

TypedArgument::TypedArgument(
    long locals_idx,
    BorrowedRef<PyTypeObject> pytype,
    int optional,
    int exact,
    Type jit_type)
    : locals_idx(locals_idx),
      optional(optional),
      exact(exact),
      jit_type(jit_type) {
  ThreadedCompileSerialize guard;
  this->pytype = Ref<PyTypeObject>::create(pytype);
  thread_safe_flags = pytype->tp_flags & kThreadSafeFlagsMask;
}

TypedArgument::~TypedArgument() {
  ThreadedCompileSerialize guard;
  pytype.release();
}

TypedArgument::TypedArgument(const TypedArgument& other)
    : locals_idx(other.locals_idx),
      optional(other.optional),
      exact(other.exact),
      jit_type(other.jit_type),
      thread_safe_flags(other.thread_safe_flags) {
  ThreadedCompileSerialize guard;
  pytype = Ref<PyTypeObject>::create(other.pytype);
}

TypedArgument& TypedArgument::operator=(const TypedArgument& other) {
  new (this) TypedArgument{other};
  return *this;
}

unsigned long TypedArgument::threadSafeTpFlags() const {
  JIT_DCHECK(
      thread_safe_flags == (pytype->tp_flags & kThreadSafeFlagsMask),
      "thread safe flags changed");
  return thread_safe_flags;
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

BorrowedRef<> Environment::addReference(Ref<> obj) {
  // ThreadedRef cannot steal from Ref, so have to go through BorrowedRef and
  // accept the extra increfs and decrefs.
  return addReference(BorrowedRef<>{obj});
}

const Environment::ReferenceSet& Environment::references() const {
  return references_;
}

bool usesRuntimeFunc([[maybe_unused]] BorrowedRef<PyCodeObject> code) {
#if PY_VERSION_HEX < 0x030C0000
  return PyTuple_GET_SIZE(PyCode_GetFreevars(code)) > 0;
#else
  // In 3.12+ we always need the runtime function because we use it to
  // initialize the _PyInterpreterFrame object.
  return true;
#endif
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
