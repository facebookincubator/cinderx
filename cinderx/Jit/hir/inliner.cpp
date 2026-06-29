// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/inliner.h"

#include "internal/pycore_code.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/preload.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinderx::jit::hir {

#define LOG_INLINER(...) JIT_LOGIF(getConfig().log.debug_inliner, __VA_ARGS__)

namespace {

struct AbstractCall {
  AbstractCall(
      BorrowedRef<PyFunctionObject> func,
      size_t nargs,
      DeoptBase* instr,
      Register* target = nullptr)
      : func{func}, nargs{nargs}, instr{instr}, target{target} {}

  Register* arg(std::size_t i) const {
    if (instr->IsInvokeStaticFunction()) {
      auto f = static_cast<InvokeStaticFunction*>(instr);
      return f->arg(i + 1);
    }
    if (instr->IsVectorCall()) {
      auto f = static_cast<VectorCall*>(instr);
      return f->arg(i);
    }
    if (instr->IsCallMethod()) {
      auto f = static_cast<CallMethod*>(instr);
      return f->arg(i);
    }
    JIT_THROW("Unsupported call type {}", instr->opname());
  }

  BorrowedRef<PyFunctionObject> func;
  size_t nargs{0};
  DeoptBase* instr{nullptr};
  Register* target{nullptr};
  // Score for ranking callsites as inlining candidates.  Lower is better.
  size_t score{0};
  // Discover order, used to break ranking ties in a stable manner.
  uint64_t seq{0};
};

void logInlineFailure(
    Function& caller,
    BorrowedRef<PyFunctionObject> callee,
    InlineFailureType failure_type) {
  std::string callee_name = funcFullname(callee);
  Function::InlineFailureStats& inline_failure_stats =
      caller.inline_function_stats.failure_stats;
  inline_failure_stats[failure_type].insert(callee_name);
  LOG_INLINER(
      "Can't inline {} into {} because {}",
      callee_name,
      caller.fullname,
      getInlineFailureMessage(failure_type));
}

void logInlineFailure(
    Function& caller,
    BorrowedRef<PyFunctionObject> callee,
    InlineFailureType failure_type,
    const char* tp_name) {
  std::string callee_name = funcFullname(callee);
  Function::InlineFailureStats& inline_failure_stats =
      caller.inline_function_stats.failure_stats;
  inline_failure_stats[failure_type].insert(callee_name);
  LOG_INLINER(
      "Can't inline {} into {} because {} but a {:.200s}",
      callee_name,
      caller.fullname,
      getInlineFailureMessage(failure_type),
      tp_name);
}

// Assigns a cost to every function, to be used when determining whether it
// makes sense to inline or not.
size_t codeCost(BorrowedRef<PyCodeObject> code) {
  // Manually iterating through the code block to count real opcodes and not
  // inline caches.  Not the best metric but it's something to start with.
  size_t num_opcodes = 0;
  for ([[maybe_unused]] auto& instr : BytecodeInstructionBlock{code}) {
    num_opcodes++;
  }
  return num_opcodes;
}

// Most of these checks are only temporary and do not in perpetuity prohibit
// inlining.
bool canInline(Function& caller, const AbstractCall& call_instr) {
  BorrowedRef<PyFunctionObject> callee = call_instr.func;

  BorrowedRef<> globals = callee->func_globals;
  if (!PyDict_Check(globals)) {
    logInlineFailure(
        caller,
        callee,
        InlineFailureType::kGlobalsNotDict,
        Py_TYPE(globals)->tp_name);
    return false;
  }

  BorrowedRef<> builtins = callee->func_builtins;
  if (!PyDict_CheckExact(builtins)) {
    logInlineFailure(
        caller,
        callee,
        InlineFailureType::kBuiltinsNotDict,
        Py_TYPE(builtins)->tp_name);
    return false;
  }

  auto fail = [&](InlineFailureType failure_type) {
    logInlineFailure(caller, callee, failure_type);
    return false;
  };

  if (callee->func_kwdefaults != nullptr) {
    return fail(InlineFailureType::kHasKwdefaults);
  }

  BorrowedRef<PyCodeObject> code{callee->func_code};
  JIT_CHECK(PyCode_Check(code), "Expected PyCodeObject");

  if (code->co_kwonlyargcount > 0) {
    return fail(InlineFailureType::kHasKwOnlyArgs);
  }
  if (code->co_flags & CO_VARARGS) {
    return fail(InlineFailureType::kHasVarargs);
  }
  if (code->co_flags & CO_VARKEYWORDS) {
    return fail(InlineFailureType::kHasVarkwargs);
  }
  JIT_DCHECK(code->co_argcount >= 0, "argcount must be positive");
  if (call_instr.nargs != static_cast<size_t>(code->co_argcount)) {
    return fail(InlineFailureType::kCalledWithMismatchedArgs);
  }
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return fail(InlineFailureType::kIsGenerator);
  }
  // Avoid the allocation that can happen in
  // PyCode_GetCellvars and PyCode_GetFreevars
  for (int offset = 0; offset < code->co_nlocalsplus; offset++) {
    _PyLocals_Kind k = _PyLocals_GetKind(code->co_localspluskinds, offset);
    if (k & CO_FAST_CELL) {
      return fail(InlineFailureType::kHasCellvars);
    } else if (k & CO_FAST_FREE) {
      return fail(InlineFailureType::kHasFreevars);
    }
  }

  // This requires access to the frame so we can't inline it.
  for (auto& bci : BytecodeInstructionBlock{code}) {
    if (bci.opcode() == EAGER_IMPORT_NAME) {
      return fail(InlineFailureType::kHasEagerImportName);
    }
  }

  return true;
}

// As canInline() for checks which require a preloader.
bool canInlineWithPreloader(
    Function& caller,
    const AbstractCall& call_instr,
    const Preloader& preloader) {
  if (call_instr.instr->IsVectorCall() &&
      (preloader.code()->co_flags & CI_CO_STATICALLY_COMPILED) &&
      (preloader.returnType() <= TPrimitive || preloader.hasPrimitiveArgs())) {
    // TASK(T122371281) remove this constraint
    logInlineFailure(
        caller,
        call_instr.func,
        InlineFailureType::kIsVectorCallWithPrimitives);
    return false;
  }

  return true;
}

// Attempt to inline a single call.  On success returns the spliced-in callee
// region (entry/exit blocks) so the caller can re-scan it for nested calls; on
// failure returns nullopt (the reason is logged into the caller's stats).
std::optional<InlineResult> inlineFunctionCall(
    Function& caller,
    const AbstractCall& call_instr) {
  if (!canInline(caller, call_instr)) {
    return std::nullopt;
  }

  auto caller_frame_state =
      std::make_unique<FrameState>(*call_instr.instr->frameState());

  BorrowedRef<PyFunctionObject> callee = call_instr.func;

  // We are only able to inline functions that were already preloaded, since we
  // can't safely preload anything mid-compile (preloading can execute arbitrary
  // Python code and raise Python exceptions). Currently this means that in
  // single-function-compile mode we are limited to inlining functions loaded as
  // globals, or statically invoked. See `preloadFuncAndDeps` for what
  // dependencies we will preload. In batch-compile mode we can inline anything
  // that is part of the batch.
  Preloader* preloader = preloaderManager().find(callee);
  if (!preloader) {
    logInlineFailure(caller, callee, InlineFailureType::kNeedsPreload);
    return std::nullopt;
  }

  if (!canInlineWithPreloader(caller, call_instr, *preloader)) {
    return std::nullopt;
  }

  HIRBuilder hir_builder(*preloader);
  std::string callee_name = funcFullname(callee);

  InlineResult result;
  try {
    result = hir_builder.inlineHIR(&caller, caller_frame_state.get());
  } catch (const std::exception& exn) {
    LOG_INLINER(
        "Tried to inline {} into {}, but failed with {}",
        callee_name,
        caller.fullname,
        exn.what());
    return std::nullopt;
  }

  // This logging is parsed by jitlist_bisect.py to find inlined functions.
  JIT_LOGIF(
      getConfig().log.debug_inliner || getConfig().log.debug,
      "Inlining function {} into {}",
      callee_name,
      caller.fullname);

  BorrowedRef<PyCodeObject> callee_code = preloader->code();
  BasicBlock* tail = caller.cfg.splitAfter(*call_instr.instr);
  auto begin_inlined_function = BeginInlinedFunction::create(
      callee, std::move(caller_frame_state), callee_name, preloader->reifier());
  auto callee_branch = Branch::create(result.entry);
  if (call_instr.target != nullptr) {
    // Not a static call. Check that __code__ has not been swapped out since
    // the function was inlined.
    // VectorCall -> {LoadField, GuardIs, BeginInlinedFunction, Branch to
    // callee CFG}
    //
    // Consider emitting a DeoptPatchpoint here to catch the case where someone
    // swaps out function.__code__.
    Register* code_obj = caller.env.AllocateRegister();
    auto load_code = LoadField::create(
        code_obj,
        call_instr.target,
        "func_code",
        offsetof(PyFunctionObject, func_code),
        TObject);
    Register* guarded_code = caller.env.AllocateRegister();
    auto guard_code = GuardIs::create(guarded_code, callee_code, code_obj);
    call_instr.instr->ExpandInto(
        {load_code, guard_code, begin_inlined_function, callee_branch});
  } else {
    call_instr.instr->ExpandInto({begin_inlined_function, callee_branch});
  }
  tail->push_front(EndInlinedFunction::create(begin_inlined_function));

  // Transform LoadArg into Assign
  for (auto it = result.entry->begin(); it != result.entry->end();) {
    auto& instr = *it;
    ++it;

    if (instr.IsLoadArg()) {
      auto load_arg = static_cast<LoadArg*>(&instr);
      auto assign =
          Assign::create(instr.output(), call_instr.arg(load_arg->arg_idx()));
      instr.ReplaceWith(*assign);
      delete &instr;
    }
  }

  // Transform Return into Assign+Branch
  auto return_instr = result.exit->GetTerminator();
  JIT_CHECK(
      return_instr->IsReturn(),
      "terminator from inlined function should be Return");
  auto assign =
      Assign::create(call_instr.instr->output(), return_instr->GetOperand(0));
  auto return_branch = Branch::create(tail);
  return_instr->ExpandInto({assign, return_branch});
  delete return_instr;

  delete call_instr.instr;
  caller.inline_function_stats.num_inlined_functions++;
  return result;
}

// Validate a dynamic call's function target and, if it names a concrete
// function we can inline, append it as a candidate.  `target` is the register
// holding the callee, `nargs` the number of positional arguments.
void maybeAddDynamicCall(
    Function& irfunc,
    DeoptBase* instr,
    Register* target,
    size_t nargs,
    CallFlags flags,
    std::vector<AbstractCall>& calls) {
  const std::string& caller_name = irfunc.fullname;
  if (!target->isA(TFunc)) {
    LOG_INLINER(
        "Can't inline non-function {}:{} into {}",
        *target,
        target->type(),
        caller_name);
    return;
  }
  if (!target->type().hasValueSpec(TFunc)) {
    LOG_INLINER(
        "Can't inline unknown function {}:{} into {}",
        *target,
        target->type(),
        caller_name);
    return;
  }
  if (flags & CallFlags::KwArgs) {
    LOG_INLINER(
        "Can't inline {}:{} into {} because it has kwargs",
        *target,
        target->type(),
        caller_name);
    return;
  }

  BorrowedRef<PyFunctionObject> callee{target->type().objectSpec()};
  calls.emplace_back(callee, nargs, instr, target);
}

// Scan a single block for calls that the inliner can potentially handle and
// append them to `calls`.  The actual inlinability of a candidate is decided
// later by canInline()/inlineFunctionCall().
void collectCalls(
    Function& irfunc,
    BasicBlock& block,
    std::vector<AbstractCall>& calls) {
  for (auto& instr : block) {
    if (instr.IsVectorCall()) {
      auto call = static_cast<VectorCall*>(&instr);
      maybeAddDynamicCall(
          irfunc, call, call->func(), call->numArgs(), call->flags(), calls);
    } else if (instr.IsCallMethod()) {
      // A CallMethod is a plain (inlinable) function call only when its
      // receiver is null; with a real receiver it's a method dispatch we can't
      // turn into a direct call.  Which operand holds the callable vs. the null
      // receiver differs by Python version (mirrors simplifyCallMethod()).  In
      // the pipeline Simplify rewrites these into VectorCalls before the
      // inliner runs, but freshly inlined callee bodies have not been through
      // Simplify yet, so we must recognize the CallMethod form directly to
      // inline transitively.
      auto call = static_cast<CallMethod*>(&instr);
      Register* target = nullptr;
      if constexpr (PY_VERSION_HEX >= 0x030E0000) {
        if (call->self()->type() <= TNullptr) {
          target = call->func();
        }
      } else {
        if (call->func()->type() <= TNullptr) {
          target = call->self();
        }
      }
      if (target != nullptr) {
        maybeAddDynamicCall(
            irfunc, call, target, call->NumArgs(), call->flags(), calls);
      }
    } else if (instr.IsInvokeStaticFunction()) {
      auto call = static_cast<InvokeStaticFunction*>(&instr);
      calls.emplace_back(call->func(), call->NumArgs() - 1, call);
    }
  }
}

// Report whether `code` already appears among the functions inlined on the path
// to a call site, walking the FrameState parent chain.  The outermost frame
// (parent == nullptr) is the function being compiled, not an inlined frame, so
// it is excluded: this still allows a directly recursive function to be inlined
// once into itself, but prevents that inlined copy (or a mutual recursion
// cycle) from being unrolled again and again.
bool inlineStackContains(
    const FrameState* frame,
    BorrowedRef<PyCodeObject> code) {
  for (; frame != nullptr && frame->parent != nullptr; frame = frame->parent) {
    if (frame->code == code) {
      return true;
    }
  }
  return false;
}

// Collect the blocks that make up a freshly inlined callee, from its entry
// block up to (and including) its single merged return block.  Traversal stops
// at `exit` so we don't walk back out into the caller's code.
std::vector<BasicBlock*> inlinedBlocks(BasicBlock* entry, BasicBlock* exit) {
  std::vector<BasicBlock*> blocks;
  std::unordered_set<BasicBlock*> seen{entry};
  std::deque<BasicBlock*> queue{entry};
  while (!queue.empty()) {
    BasicBlock* block = queue.front();
    queue.pop_front();
    blocks.push_back(block);
    if (block == exit) {
      continue;
    }
    Instr* terminator = block->GetTerminator();
    for (std::size_t i = 0, n = terminator->numEdges(); i < n; i++) {
      BasicBlock* succ = block->successor(i);
      if (seen.insert(succ).second) {
        queue.push_back(succ);
      }
    }
  }
  return blocks;
}

void tryEliminateBeginEnd(EndInlinedFunction* end) {
  BeginInlinedFunction* begin = end->matchingBegin();
  if (begin->block() != end->block()) {
    // Elimination across basic blocks not supported yet.
    return;
  }
  auto it = begin->block()->iterator_to(*begin);
  it++;
  std::vector<Instr*> to_delete{begin, end};
  for (; &*it != end; it++) {
    // Snapshots reference the FrameState owned by BeginInlinedFunction and, if
    // not removed, will contain bad pointers.
    if (it->IsSnapshot()) {
      to_delete.push_back(&*it);
      continue;
    }
    // Instructions that either deopt or otherwise materialize a PyFrameObject
    // need the inline frames to exist.  Everything that materializes a
    // PyFrameObject should also be marked as deopting.  Updating the previous
    // instruction needs the frame too.
    if (it->asDeoptBase() || hasArbitraryExecution(*it)) {
      return;
    }
  }
  for (Instr* instr : to_delete) {
    instr->unlink();
    delete instr;
  }
}

} // namespace

void InlineFunctionCalls::Run(Function& irfunc) {
  if (irfunc.code == nullptr) {
    // In tests, irfunc may not have bytecode.
    return;
  }
  if (irfunc.code->co_flags & kCoFlagsAnyGenerator) {
    // TASK(T109706798): Support inlining into generators
    LOG_INLINER(
        "Refusing to inline functions into {}: function is a generator",
        irfunc.fullname);
    return;
  }

  const size_t cost_limit = getConfig().inliner_cost_limit;
  const size_t depth_limit = getConfig().inliner_depth_limit;
  const size_t cold_threshold = getConfig().inliner_cold_call_threshold;
  size_t cost = codeCost(irfunc.code);

  // Priority queue of candidate calls, ordered so that smaller callees are
  // inlined first.  Preferring small callees lets us fit more of them under the
  // cost limit, which matters once a large function starts hitting it.  Ties
  // are broken by discovery order to keep inlining stable and source-ordered.
  auto lowerPriority = [](const AbstractCall& a, const AbstractCall& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.seq > b.seq;
  };
  std::priority_queue<
      AbstractCall,
      std::vector<AbstractCall>,
      decltype(lowerPriority)>
      queue{lowerPriority};
  uint64_t seq = 0;

  // Process the calls found in the caller's code and push the ones worth
  // inlining onto the queue.
  auto enqueueCandidates = [&](PyCodeObject* caller_code,
                               const std::string& caller_name,
                               const std::vector<AbstractCall>& candidates) {
    size_t caller_count = codeCallCount(caller_code);
    for (AbstractCall call : candidates) {
      BorrowedRef<PyCodeObject> callee_code{call.func->func_code};

      // Prune out callees that are substantially colder than the caller.  Don't
      // prune anything when a caller hasn't been run yet (e.g. compiled via
      // cinderx.jit.force_compile(), or via a JIT list).
      if (caller_count != 0) {
        size_t callee_count = codeCallCount(callee_code);
        if (callee_count == 0 ||
            caller_count / callee_count >= cold_threshold) {
          LOG_INLINER(
              "Pruning cold call to {} from {}: callee called {} times vs "
              "caller's {}",
              funcFullname(call.func),
              caller_name,
              callee_count,
              caller_count);
          continue;
        }
      }

      // Rank callees by their size, smaller calls are cheaper to inline.
      call.score = codeCost(callee_code);
      call.seq = seq++;
      queue.push(call);
    }
  };

  // Seed the queue with the top-level function's callsites.  We grow it
  // transitively: whenever we splice in a callee we re-scan its body so that
  // the callee's own calls become candidates too.
  {
    std::vector<AbstractCall> candidates;
    for (auto& block : irfunc.cfg.blocks) {
      collectCalls(irfunc, block, candidates);
    }
    enqueueCandidates(irfunc.code, irfunc.fullname, candidates);
  }

  while (!queue.empty()) {
    AbstractCall call = queue.top();
    queue.pop();

    BorrowedRef<PyCodeObject> call_code{call.func->func_code};
    const FrameState* call_site = call.instr->frameState();
    // Inline depth of the call site.  A top-level call site is at depth 0, so
    // the function we'd inline there lands at depth 1.
    const size_t inline_depth = call_site->inlineDepth();

    // Don't unroll directly or mutually recursive calls.
    if (inlineStackContains(call_site, call_code)) {
      logInlineFailure(irfunc, call.func, InlineFailureType::kIsRecursive);
      continue;
    }

    // Bound how deep transitive inlining can go.
    if (inline_depth >= depth_limit) {
      logInlineFailure(
          irfunc, call.func, InlineFailureType::kExceedsDepthLimit);
      continue;
    }

    // Charge the callee's size against the budget.
    size_t new_cost = cost + codeCost(call_code);
    if (new_cost > cost_limit) {
      LOG_INLINER(
          "Inliner reached cost limit of {} when trying to inline {} into {}, "
          "skipping",
          new_cost,
          funcFullname(call.func),
          irfunc.fullname);
      continue;
    }

    std::optional<InlineResult> result = inlineFunctionCall(irfunc, call);
    if (!result.has_value()) {
      // Inlining failed; the reason has been logged.  Don't charge its cost.
      continue;
    }
    cost = new_cost;

    // We need to reflow types after every inline to propagate new type
    // information from the callee.  This also gives the newly inlined call
    // targets the value specs that collectCalls() relies on.
    reflowTypes(irfunc);

    // Re-scan the just-inlined body so calls it makes become candidates too,
    // ranking and pruning them relative to the callee we just inlined.
    std::vector<AbstractCall> nested;
    for (BasicBlock* block : inlinedBlocks(result->entry, result->exit)) {
      collectCalls(irfunc, *block, nested);
    }
    enqueueCandidates(call_code, funcFullname(call.func), nested);
  }

  // The inliner will make some blocks unreachable and we need to remove them to
  // make the CFG valid again.  While inlining might make some blocks
  // unreachable and therefore make less work (less to inline), we cannot remove
  // unreachable blocks in the above loop.  It might delete instructions pointed
  // to by `calls`.
  removeUnreachableBlocks(irfunc);
  CopyPropagation{}.Run(irfunc);
  CleanCFG{}.Run(irfunc);
}

void BeginInlinedFunctionElimination::Run(Function& irfunc) {
  std::vector<EndInlinedFunction*> ends;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.IsEndInlinedFunction()) {
        continue;
      }
      ends.push_back(static_cast<EndInlinedFunction*>(&instr));
    }
  }
  for (EndInlinedFunction* end : ends) {
    tryEliminateBeginEnd(end);
  }
}

} // namespace cinderx::jit::hir
