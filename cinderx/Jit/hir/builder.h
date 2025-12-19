// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/preload.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

class BasicBlock;
class Environment;
class Function;
class Register;

// Helper class for managing temporary variables
class TempAllocator {
 public:
  explicit TempAllocator(Environment* env) : env_(env) {}

  // Allocate a temp register that may be used for the stack. It should not be a
  // register that will be treated specially in the FrameState (e.g. tracked as
  // containing a local or cell.)
  Register* AllocateStack();

  // Get the i-th stack temporary or allocate one
  Register* GetOrAllocateStack(std::size_t idx);

  // Allocate a temp register that will not be used for a stack value.
  Register* AllocateNonStack();

 private:
  Environment* env_;
  std::vector<Register*> cache_;
};

// We expect that on exit from a basic block the stack only contains temporaries
// in increasing order (called the canonical form). For example,
//
//    t0
//    t1
//    t2 <- top of stack
//
// It may be the case that temporaries are re-ordered, duplicated, or the stack
// contains locals. This class is responsible for inserting the necessary
// register moves such that the stack is in canonical form.
class BlockCanonicalizer {
 public:
  BlockCanonicalizer() : processing_(), done_(), copies_(), moved_() {}

  void Run(BasicBlock* block, TempAllocator& temps, OperandStack& stack);

 private:
  DISALLOW_COPY_AND_ASSIGN(BlockCanonicalizer);

  void InsertCopies(
      Register* reg,
      TempAllocator& temps,
      Instr& terminator,
      std::vector<Register*>& alloced);

  std::unordered_set<Register*> processing_;
  std::unordered_set<Register*> done_;
  std::unordered_map<Register*, std::vector<Register*>> copies_;
  std::unordered_map<Register*, Register*> moved_;
};

// Translate the bytecode for preloader.code into HIR, in the context of the
// preloaded globals and classloader lookups in the preloader.
//
// The resulting HIR is un-optimized, not in SSA form, and does not yet have
// refcount operations or types flowed through it. Later passes will transform
// to SSA, flow types, optimize, and insert refcount operations using liveness
// analysis.
std::unique_ptr<Function> buildHIR(const Preloader& preloader);

// Inlining merges all of the different callee Returns (which terminate blocks,
// leading to a bunch of distinct exit blocks) into Branches to one Return
// block (one exit block), which the caller can transform into an Assign to the
// output register of the original call instruction.
struct InlineResult {
  BasicBlock* entry{nullptr};
  BasicBlock* exit{nullptr};
};

class HIRBuilder {
 public:
  explicit HIRBuilder(const Preloader& preloader)
      : code_(preloader.code()), preloader_(preloader) {}

  // Translate the bytecode for code_ into HIR, in the context of the preloaded
  // globals and classloader lookups from preloader_.
  //
  // The resulting HIR is un-optimized, not in SSA form, and does not yet have
  // refcount operations or types flowed through it. Later passes will transform
  // to SSA, flow types, optimize, and insert refcount operations using liveness
  // analysis.
  std::unique_ptr<Function> buildHIR();

  // Given the preloader for the callee (passed into the constructor),
  // construct the CFG for the callee in the caller's CFG. Does not link the
  // two CFGs, except for FrameState parent pointers.  Use caller_frame_state
  // as the starting FrameState for the callee.
  //
  // Use InlineResult::succeeded to check if inlining succeeded.
  InlineResult inlineHIR(Function* caller, FrameState* caller_frame_state);

 private:
  DISALLOW_COPY_AND_ASSIGN(HIRBuilder);

  // Used by buildHIR and inlineHIR.
  // irfunc is the function being compiled or the caller function.
  // frame_state should be nullptr if irfunc matches the preloader (not
  // inlining) and non-nullptr otherwise (inlining).
  // Returns the entry block.
  BasicBlock* buildHIRImpl(Function* irfunc, FrameState* frame_state);

  struct TranslationContext;
  void translate(
      Function& irfunc,
      const jit::BytecodeInstructionBlock& bc_instrs,
      const TranslationContext& tc);

  void emitPushNull(TranslationContext& tc);

  void emitBinaryOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitUnaryNot(TranslationContext& tc);
  void emitUnaryOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitAnyCall(
      CFG& cfg,
      TranslationContext& tc,
      jit::BytecodeInstructionBlock::Iterator& bc_it,
      const jit::BytecodeInstructionBlock& bc_instrs);
  void emitCallEx(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr,
      CallFlags flags);
  void emitCallInstrinsic(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitResume(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitKwNames(TranslationContext& tc, const BytecodeInstruction& bc_instr);
  void emitIsOp(TranslationContext& tc, int oparg);
  void emitContainsOp(TranslationContext& tc, int oparg);
  void emitCompareOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitToBool(TranslationContext& tc);
  void emitCopyDictWithoutKeys(TranslationContext& tc);
  void emitGetLen(TranslationContext& tc);
  void emitJumpIf(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitDeleteAttr(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadAttr(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadMethod(TranslationContext& tc, int name_idx);
  void emitLoadMethodOrAttrSuper(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr,
      bool load_method);
  void emitCopy(TranslationContext& tc, int item_idx);
  void emitCopyFreeVars(TranslationContext& tc, int nfreevars);
  void emitSwap(TranslationContext& tc, int item_idx);
  void emitMakeCell(TranslationContext& tc, int local_idx);
  void emitLoadDeref(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreDeref(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadAssertionError(TranslationContext& tc, Environment& env);
  void emitLoadClass(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadConst(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadFast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadFastLoadFast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadGlobal(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadType(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitMakeFunction(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitFunctionCredential(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitMakeListTuple(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildCheckedList(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildCheckedMap(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildMap(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildSet(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildConstKeyMap(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPopJumpIf(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPopJumpIfNone(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreAttr(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreFast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreFastStoreFast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreFastLoadFast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBinarySlice(TranslationContext& tc);
  void emitStoreSlice(TranslationContext& tc);
  void emitStoreSubscr(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitInPlaceOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitBuildSlice(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadIterableArg(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  bool emitInvokeFunction(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr,
      CallFlags flags);
  bool emitInvokeNative(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitGetIter(TranslationContext& tc);
  void emitGetYieldFromIter(CFG& cfg, TranslationContext& tc);
  void emitListAppend(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);
  void emitListExtend(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitListToTuple(TranslationContext& tc);
  void emitForIter(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitInvokeMethodVectorCall(
      TranslationContext& tc,
      bool is_awaited,
      std::vector<Register*>& arg_regs,
      const InvokeTarget& target);
  void emitLoadMethodStatic(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  bool emitInvokeMethod(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr,
      bool is_awaited);
  void emitLoadField(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreField(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitCast(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitTpAlloc(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadSmallInt(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitStoreLocal(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitLoadLocal(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitConvertPrimitive(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveLoadConst(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitIntLoadConstOld(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveBinaryOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveCompare(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveBox(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveUnbox(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitImportFrom(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitImportName(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitPrimitiveUnaryOp(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitFastLen(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitRaiseVarargs(TranslationContext& tc);
  void emitRefineType(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSequenceGet(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSequenceRepeat(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSequenceSet(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitYieldValue(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitGetAwaitable(
      CFG& cfg,
      TranslationContext& tc,
      const BytecodeInstructionBlock& bc_instrs,
      BytecodeInstruction bc_instr);
  void emitUnpackEx(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitUnpackSequence(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSetupFinally(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitAsyncForHeaderYieldFrom(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitEndAsyncFor(TranslationContext& tc);
  void emitGetAIter(TranslationContext& tc);
  void emitGetANext(TranslationContext& tc);
  Register* emitSetupWithCommon(
      TranslationContext& tc,
#if PY_VERSION_HEX < 0x030C0000
      _Py_Identifier* enter_id,
      _Py_Identifier* exit_id,
#else
      PyObject* enter_id,
      PyObject* exit_id,
#endif
      bool is_async);
  void emitBeforeWith(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSetupAsyncWith(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSetupWith(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitYieldFrom(TranslationContext& tc, Register* out);
  void emitDispatchEagerCoroResult(
      CFG& cfg,
      TranslationContext& tc,
      Register* out,
      BasicBlock* await_block,
      BasicBlock* post_await_block);

  void emitBuildString(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitFormatValue(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitFormatWithSpec(TranslationContext& tc);

  void emitMapAdd(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSetAdd(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitSetUpdate(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);

  void
  emitMatchMappingSequence(CFG& cfg, TranslationContext& tc, uint64_t tf_flag);

  void emitMatchClass(
      CFG& cfg,
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);
  void emitMatchKeys(CFG& cfg, TranslationContext& tc);

  void emitDictUpdate(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);
  void emitDictMerge(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);

  void emitSend(TranslationContext& tc, const BytecodeInstruction& bc_instr);

  void emitSetFunctionAttribute(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);

  void emitTypeAnnotationGuards(TranslationContext& tc);

  void emitBuildInterpolation(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);

  void emitBuildTemplate(TranslationContext& tc);

  void emitConvertValue(
      TranslationContext& tc,
      const jit::BytecodeInstruction& bc_instr);

  void emitFormatSimple(CFG& cfg, TranslationContext& tc);

  void emitLoadCommonConstant(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);

  void emitLoadSpecial(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);

  void emitLoadBuildClass(TranslationContext& tc);

  void emitStoreGlobal(
      TranslationContext& tc,
      const BytecodeInstruction& bc_instr);

  BorrowedRef<> constArg(const jit::BytecodeInstruction& bc_instr);

  ExecutionBlock popBlock(CFG& cfg, TranslationContext& tc);
  void insertEvalBreakerCheckForLoop(CFG& cfg, BasicBlock* loop_header);
  void insertEvalBreakerCheckForExcept(CFG& cfg, TranslationContext& tc);
  void insertEvalBreakerCheck(
      CFG& cfg,
      BasicBlock* check_block,
      BasicBlock* succ,
      const FrameState& frame);
  void addInitialYield(TranslationContext& tc);
  void addLoadArgs(TranslationContext& tc, int num_args);
  void addInitializeCells(TranslationContext& tc);
  void allocateLocalsplus(Environment* env, FrameState& state);
  void moveOverwrittenStackRegisters(TranslationContext& tc, Register* dst);
  bool tryEmitDirectMethodCall(
      const InvokeTarget& target,
      TranslationContext& tc,
      long nargs);
  bool isStaticRand(const InvokeTarget& target);
  bool tryEmitStaticRandCall(
      const InvokeTarget& target,
      TranslationContext& tc,
      long nargs);
  struct BlockMap {
    std::unordered_map<BCOffset, BasicBlock*> blocks;
    std::unordered_map<BasicBlock*, BytecodeInstructionBlock> bc_blocks;
  };
  BlockMap createBlocks(
      Function& irfunc,
      const BytecodeInstructionBlock& bc_block);
  BasicBlock* getBlockAtOff(BCOffset off);

  // When a static function calls another static function indirectly, all args
  // are passed boxed and the return value will come back boxed, so we must
  // box primitive args and and unbox primitive return values. These functions
  // take care of these two, respectively.
  std::vector<Register*> setupStaticArgs(
      TranslationContext& tc,
      const InvokeTarget& target,
      long nargs,
      bool statically_invoked);
  void fixStaticReturn(TranslationContext& tc, Register* reg, Type ret_type);

  // Box the primitive value from src into dst, using the given type.
  void
  boxPrimitive(TranslationContext& tc, Register* dst, Register* src, Type type);

  // Unbox the primitive value from src into dst, using the given type. Similar
  // to TranslationContext::emitChecked(), but uses IsNegativeAndErrOccurred
  // instead of the normal CheckExc because of the primitive output value.
  void unboxPrimitive(
      TranslationContext& tc,
      Register* dst,
      Register* src,
      Type type);

  // Check that a code object can be compiled into HIR.
  void checkTranslate();

  void advancePastYieldInstr(TranslationContext& tc);

  BorrowedRef<PyCodeObject> code_;
  BlockMap block_map_;
  const Preloader& preloader_;

  TempAllocator temps_{nullptr};

  // Tracks the function for compilations that require it.
  Register* func_{nullptr};

  // Tracks the most recent constant read from a KW_NAMES opcode.
  Register* kwnames_{nullptr};

  OperandStack static_method_stack_;
};

} // namespace jit::hir
