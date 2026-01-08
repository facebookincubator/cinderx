// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/inliner.h"

#include "internal/pycore_code.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/preload.h"

namespace jit::hir {

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
    JIT_ABORT("Unsupported call type {}", instr->opname());
  }

  BorrowedRef<PyFunctionObject> func;
  size_t nargs{0};
  DeoptBase* instr{nullptr};
  Register* target{nullptr};
};

void dlogAndCollectFailureStats(
    Function& caller,
    AbstractCall* call_instr,
    InlineFailureType failure_type) {
  BorrowedRef<PyFunctionObject> func = call_instr->func;
  std::string callee_name = funcFullname(func);
  Function::InlineFailureStats& inline_failure_stats =
      caller.inline_function_stats.failure_stats;
  inline_failure_stats[failure_type].insert(callee_name);
  LOG_INLINER(
      "Can't inline {} into {} because {}",
      callee_name,
      caller.fullname,
      getInlineFailureMessage(failure_type));
}

void dlogAndCollectFailureStats(
    Function& caller,
    AbstractCall* call_instr,
    InlineFailureType failure_type,
    const char* tp_name) {
  BorrowedRef<PyFunctionObject> func = call_instr->func;
  std::string callee_name = funcFullname(func);
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
bool canInline(Function& caller, AbstractCall* call_instr) {
  BorrowedRef<PyFunctionObject> func = call_instr->func;

  BorrowedRef<> globals = func->func_globals;
  if (!PyDict_Check(globals)) {
    dlogAndCollectFailureStats(
        caller,
        call_instr,
        InlineFailureType::kGlobalsNotDict,
        Py_TYPE(globals)->tp_name);
    return false;
  }

  BorrowedRef<> builtins = func->func_builtins;
  if (!PyDict_CheckExact(builtins)) {
    dlogAndCollectFailureStats(
        caller,
        call_instr,
        InlineFailureType::kBuiltinsNotDict,
        Py_TYPE(builtins)->tp_name);
    return false;
  }

  auto fail = [&](InlineFailureType failure_type) {
    dlogAndCollectFailureStats(caller, call_instr, failure_type);
    return false;
  };

  if (func->func_kwdefaults != nullptr) {
    return fail(InlineFailureType::kHasKwdefaults);
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
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
  if (call_instr->nargs != static_cast<size_t>(code->co_argcount)) {
    return fail(InlineFailureType::kCalledWithMismatchedArgs);
  }
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return fail(InlineFailureType::kIsGenerator);
  }
#if PY_VERSION_HEX >= 0x030C0000
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
#else
  Py_ssize_t ncellvars = PyTuple_GET_SIZE(PyCode_GetCellvars(code));
  if (ncellvars > 0) {
    return fail(InlineFailureType::kHasCellvars);
  }
  Py_ssize_t nfreevars = PyTuple_GET_SIZE(PyCode_GetFreevars(code));
  if (nfreevars > 0) {
    return fail(InlineFailureType::kHasFreevars);
  }
#endif

  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    // This requires access to the frame so we can't inline it.
    for (auto& bci : BytecodeInstructionBlock{code}) {
      if (bci.opcode() == EAGER_IMPORT_NAME) {
        return fail(InlineFailureType::kHasEagerImportName);
      }
    }
  }

  return true;
}

// As canInline() for checks which require a preloader.
bool canInlineWithPreloader(
    Function& caller,
    AbstractCall* call_instr,
    const Preloader& preloader) {
  if (call_instr->instr->IsVectorCall() &&
      (preloader.code()->co_flags & CI_CO_STATICALLY_COMPILED) &&
      (preloader.returnType() <= TPrimitive || preloader.hasPrimitiveArgs())) {
    // TASK(T122371281) remove this constraint
    dlogAndCollectFailureStats(
        caller, call_instr, InlineFailureType::kIsVectorCallWithPrimitives);
    return false;
  }

  return true;
}

void inlineFunctionCall(Function& caller, AbstractCall* call_instr) {
  if (!canInline(caller, call_instr)) {
    return;
  }

  auto caller_frame_state =
      std::make_unique<FrameState>(*call_instr->instr->frameState());

  BorrowedRef<PyFunctionObject> callee = call_instr->func;

  // We are only able to inline functions that were already preloaded, since we
  // can't safely preload anything mid-compile (preloading can execute arbitrary
  // Python code and raise Python exceptions). Currently this means that in
  // single-function-compile mode we are limited to inlining functions loaded as
  // globals, or statically invoked. See `preloadFuncAndDeps` for what
  // dependencies we will preload. In batch-compile mode we can inline anything
  // that is part of the batch.
  Preloader* preloader = preloaderManager().find(callee);
  if (!preloader) {
    dlogAndCollectFailureStats(
        caller, call_instr, InlineFailureType::kNeedsPreload);
    return;
  }

  if (!canInlineWithPreloader(caller, call_instr, *preloader)) {
    return;
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
    return;
  }

  // This logging is parsed by jitlist_bisect.py to find inlined functions.
  JIT_LOGIF(
      getConfig().log.debug_inliner || getConfig().log.debug,
      "Inlining function {} into {}",
      callee_name,
      caller.fullname);

  BorrowedRef<PyCodeObject> callee_code{callee->func_code};
  BasicBlock* tail = caller.cfg.splitAfter(*call_instr->instr);
  auto begin_inlined_function = BeginInlinedFunction::create(
      callee, std::move(caller_frame_state), callee_name, preloader->reifier());
  auto callee_branch = Branch::create(result.entry);
  if (call_instr->target != nullptr) {
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
        call_instr->target,
        "func_code",
        offsetof(PyFunctionObject, func_code),
        TObject);
    Register* guarded_code = caller.env.AllocateRegister();
    auto guard_code = GuardIs::create(guarded_code, callee_code, code_obj);
    call_instr->instr->ExpandInto(
        {load_code, guard_code, begin_inlined_function, callee_branch});
  } else {
    call_instr->instr->ExpandInto({begin_inlined_function, callee_branch});
  }
  tail->push_front(EndInlinedFunction::create(begin_inlined_function));

  // Transform LoadArg into Assign
  for (auto it = result.entry->begin(); it != result.entry->end();) {
    auto& instr = *it;
    ++it;

    if (instr.IsLoadArg()) {
      auto load_arg = static_cast<LoadArg*>(&instr);
      auto assign =
          Assign::create(instr.output(), call_instr->arg(load_arg->arg_idx()));
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
      Assign::create(call_instr->instr->output(), return_instr->GetOperand(0));
  auto return_branch = Branch::create(tail);
  return_instr->ExpandInto({assign, return_branch});
  delete return_instr;

  delete call_instr->instr;
  caller.inline_function_stats.num_inlined_functions++;
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
    // need the shadow frames to exist. Everything that materializes a
    // PyFrameObject should also be marked as deopting.

    if (it->asDeoptBase()
#if PY_VERSION_HEX >= 0x030C0000
        // Updating the previous instruction needs the frame too.
        || hasArbitraryExecution(*it)
#endif
    ) {
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

  // Scan through all function calls in `irfunc` and mark the ones that are
  // suitable for inlining.
  std::vector<AbstractCall> to_inline;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsVectorCall()) {
        auto call = static_cast<VectorCall*>(&instr);
        Register* target = call->func();
        const std::string& caller_name = irfunc.fullname;
        if (!target->isA(TFunc)) {
          LOG_INLINER(
              "Can't inline non-function {}:{} into {}",
              *target,
              target->type(),
              caller_name);
          continue;
        }
        if (!target->type().hasValueSpec(TFunc)) {
          LOG_INLINER(
              "Can't inline unknown function {}:{} into {}",
              *target,
              target->type(),
              caller_name);
          continue;
        }
        if (call->flags() & CallFlags::KwArgs) {
          LOG_INLINER(
              "Can't inline {}:{} into {} because it has kwargs",
              *target,
              target->type(),
              caller_name);
          continue;
        }

        BorrowedRef<PyFunctionObject> callee{target->type().objectSpec()};
        to_inline.emplace_back(callee, call->numArgs(), call, target);
      } else if (instr.IsInvokeStaticFunction()) {
        auto call = static_cast<InvokeStaticFunction*>(&instr);
        to_inline.emplace_back(call->func(), call->NumArgs() - 1, call);
      }
    }
  }

  if (to_inline.empty()) {
    return;
  }

  size_t cost_limit = getConfig().inliner_cost_limit;
  size_t cost = codeCost(irfunc.code);

  // Inline as many calls as possible, starting from the top of the function and
  // working down.
  for (auto& call : to_inline) {
    BorrowedRef<PyCodeObject> call_code{call.func->func_code};
    size_t new_cost = cost + codeCost(call_code);
    if (new_cost > cost_limit) {
      LOG_INLINER(
          "Inliner reached cost limit of {} when trying to inline {} into {}, "
          "inlining stopping early",
          new_cost,
          funcFullname(call.func),
          irfunc.fullname);
      break;
    }
    cost = new_cost;

    inlineFunctionCall(irfunc, &call);

    // We need to reflow types after every inline to propagate new type
    // information from the callee.
    reflowTypes(irfunc);
  }

  // The inliner will make some blocks unreachable and we need to remove them
  // to make the CFG valid again. While inlining might make some blocks
  // unreachable and therefore make less work (less to inline), we cannot
  // remove unreachable blocks in the above loop. It might delete instructions
  // pointed to by `to_inline`.
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

} // namespace jit::hir
