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

#include <algorithm>
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
    if (instr->isInvokeStaticFunction()) {
      auto f = static_cast<InvokeStaticFunction*>(instr);
      return f->arg(i + 1);
    }
    if (instr->isVectorCall()) {
      auto f = static_cast<VectorCall*>(instr);
      return f->arg(i);
    }
    if (instr->isCallMethod()) {
      auto f = static_cast<CallMethod*>(instr);
      return f->arg(i);
    }
    JIT_THROW("Unsupported call type {}", instr->opname());
  }

  BorrowedRef<PyFunctionObject> func;
  // Number of call operands excluding the callee itself. For KwArgs calls
  // this includes the keyword values plus the trailing kwnames tuple.
  size_t nargs{0};
  DeoptBase* instr{nullptr};
  Register* target{nullptr};
  // Score for ranking callsites as inlining candidates.  Lower is better.
  size_t score{0};
  // Discover order, used to break ranking ties in a stable manner.
  uint64_t seq{0};
};

// Whether a call passes keyword arguments (kwnames tuple as last operand).
// Static calls never carry kwargs.
bool callHasKwArgs(const AbstractCall& call_instr) {
  const DeoptBase* instr = call_instr.instr;
  CallFlags flags = CallFlags::None;
  if (instr->isVectorCall()) {
    flags = instr->as<VectorCall>().flags();
  } else if (instr->isCallMethod()) {
    flags = instr->as<CallMethod>().flags();
  }
  return flags & CallFlags::KwArgs;
}

bool unicodeNameEquals(PyObject* a, PyObject* b) {
  if (a == b) {
    return true;
  }
  if (!PyUnicode_Check(a) || !PyUnicode_Check(b)) {
    return false;
  }
  return PyUnicode_Compare(a, b) == 0;
}

// Call operands resolved onto callee parameters. param_regs holds the
// register for each positional (co_argcount entries) then kwonly
// (co_kwonlyargcount entries) parameter, or nullptr when the call provides
// no value for it.
struct MappedCallArgs {
  // Number of positional values (excluding kwnames).
  size_t num_pos = 0;
  // Number of keyword values.
  size_t num_kw = 0;
  std::vector<Register*> param_regs;
};

// Map a call's operands onto the callee's parameters. Returns nullopt when
// the call cannot be mapped (kwnames not constant, unknown or duplicate
// keywords, keyword for a positional-only parameter, ...).
std::optional<MappedCallArgs> mapCallArgs(
    BorrowedRef<PyCodeObject> code,
    const AbstractCall& call_instr) {
  MappedCallArgs result;
  const size_t co_argcount = static_cast<size_t>(code->co_argcount);
  const size_t co_kwonly = static_cast<size_t>(code->co_kwonlyargcount);
  result.param_regs.assign(co_argcount + co_kwonly, nullptr);
  if (!callHasKwArgs(call_instr)) {
    result.num_pos = call_instr.nargs;
    for (size_t i = 0; i < co_argcount && i < call_instr.nargs; ++i) {
      result.param_regs[i] = call_instr.arg(i);
    }
    return result;
  }
  if (call_instr.nargs < 1) {
    return std::nullopt;
  }
  // The last operand is the kwnames tuple; it must be a compile-time
  // constant for us to map keywords to parameters.
  Register* kwnames_reg = call_instr.arg(call_instr.nargs - 1);
  const Type kwnames_type = kwnames_reg->type();
  if (!kwnames_type.hasObjectSpec()) {
    LOG_INLINER("Can't inline call with non-constant kwnames");
    return std::nullopt;
  }
  BorrowedRef<PyObject> kwnames{kwnames_type.objectSpec()};
  if (!PyTuple_Check(kwnames)) {
    return std::nullopt;
  }
  const size_t num_kw = static_cast<size_t>(PyTuple_GET_SIZE(kwnames));
  if (call_instr.nargs < num_kw + 1) {
    return std::nullopt;
  }
  result.num_kw = num_kw;
  result.num_pos = call_instr.nargs - 1 - num_kw;
  const size_t num_pos = result.num_pos;
  for (size_t i = 0; i < num_pos && i < co_argcount; ++i) {
    result.param_regs[i] = call_instr.arg(i);
  }
  const size_t posonly = static_cast<size_t>(code->co_posonlyargcount);
  if (co_argcount + co_kwonly > static_cast<size_t>(code->co_nlocalsplus)) {
    return std::nullopt;
  }
  for (size_t j = 0; j < num_kw; ++j) {
    BorrowedRef<PyObject> name{PyTuple_GET_ITEM(kwnames.get(), j)};
    if (!PyUnicode_Check(name)) {
      return std::nullopt;
    }
    bool matched = false;
    // Positional-only parameters (before posonly) cannot be filled by
    // keyword; anything else unfilled may be.
    for (size_t idx = posonly; idx < co_argcount + co_kwonly; ++idx) {
      BorrowedRef<PyObject> param_name{getVarname(code, static_cast<int>(idx))};
      if (!unicodeNameEquals(name, param_name)) {
        continue;
      }
      if (result.param_regs[idx] != nullptr) {
        LOG_INLINER("Can't inline call passing a duplicate argument");
        return std::nullopt;
      }
      result.param_regs[idx] = call_instr.arg(num_pos + j);
      matched = true;
      break;
    }
    if (!matched) {
      LOG_INLINER("Can't inline call with unknown keyword argument");
      return std::nullopt;
    }
  }
  return result;
}

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
  // Use the number of opcodes as the cost.  Not the best metric but it's
  // something to start with.
  BytecodeInstructionBlock instructions{code};
  return std::distance(instructions.begin(), instructions.end());
}

// Most of these checks are only temporary and do not in perpetuity prohibit
// inlining.
// Validate a call for inlining. On success returns the argument mapping
// computed by mapCallArgs() so inlineFunctionCall() can reuse it without
// re-reading caller registers (whose types inlineHIR() may invalidate
// while splicing in the callee). On failure returns nullopt.
std::optional<MappedCallArgs> canInline(
    Function& caller,
    const AbstractCall& call_instr,
    BorrowedRef<PyTupleObject> func_defaults) {
  BorrowedRef<PyFunctionObject> callee = call_instr.func;

  BorrowedRef<> globals = callee->func_globals;
  if (!PyDict_Check(globals)) {
    logInlineFailure(
        caller,
        callee,
        InlineFailureType::kGlobalsNotDict,
        Py_TYPE(globals)->tp_name);
    return std::nullopt;
  }

  BorrowedRef<> builtins = callee->func_builtins;
  if (!PyDict_CheckExact(builtins)) {
    logInlineFailure(
        caller,
        callee,
        InlineFailureType::kBuiltinsNotDict,
        Py_TYPE(builtins)->tp_name);
    return std::nullopt;
  }

  auto fail =
      [&](InlineFailureType failure_type) -> std::optional<MappedCallArgs> {
    logInlineFailure(caller, callee, failure_type);
    return std::nullopt;
  };

  // kwdefaults are only read when a kwonly parameter lacks a value; with a
  // fully-provided kwargs call (see below) they are never consulted.
  const bool call_has_kwargs = callHasKwArgs(call_instr);
  if (callee->func_kwdefaults != nullptr && !call_has_kwargs) {
    return fail(InlineFailureType::kHasKwdefaults);
  }

  BorrowedRef<PyCodeObject> code{callee->func_code};
  JIT_CHECK(PyCode_Check(code), "Expected PyCodeObject");

  JIT_DCHECK(code->co_argcount >= 0, "argcount must be positive");
  JIT_DCHECK(code->co_kwonlyargcount >= 0, "kwonlyargcount must be positive");
  if (code->co_kwonlyargcount > 0 && !call_has_kwargs) {
    return fail(InlineFailureType::kHasKwOnlyArgs);
  }
  if (code->co_flags & CO_VARKEYWORDS) {
    return fail(InlineFailureType::kHasVarkwargs);
  }
  // Map call operands onto callee parameters. A callee with *args accepts
  // surplus positional arguments, which are collected into the varargs
  // tuple when rewriting LoadArg below.
  auto mapping = mapCallArgs(code, call_instr);
  if (!mapping) {
    return fail(InlineFailureType::kCalledWithMismatchedArgs);
  }
  const bool has_varargs = code->co_flags & CO_VARARGS;
  const size_t co_argcount = static_cast<size_t>(code->co_argcount);
  if (mapping->num_pos > co_argcount && !has_varargs) {
    return fail(InlineFailureType::kCalledWithMismatchedArgs);
  }
  if (mapping->num_kw) {
    // Only support kwargs calls when every argument is provided; defaults
    // are never consulted.
    for (Register* reg : mapping->param_regs) {
      if (reg == nullptr) {
        return fail(InlineFailureType::kCalledWithMismatchedArgs);
      }
    }
  } else {
    // Grab default positional arguments, mirroring resolveArgs in
    // simplify.cpp. func_defaults comes from the callee's preloader
    // (captured with the GIL held and kept alive); reading it off the
    // function here would race with concurrent reassignment on a
    // background compile without the GIL.
    const size_t num_defaults = func_defaults == nullptr
        ? 0
        : static_cast<size_t>(PyTuple_GET_SIZE(func_defaults));
    if (mapping->num_pos + num_defaults < co_argcount) {
      // Function was called with too few arguments.
      return fail(InlineFailureType::kCalledWithMismatchedArgs);
    }
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

  return mapping;
}

// As canInline() for checks which require a preloader.
bool canInlineWithPreloader(
    Function& caller,
    const AbstractCall& call_instr,
    const Preloader& preloader) {
  if (call_instr.instr->isVectorCall() &&
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

Register* populateVarArgs(
    Function& caller,
    const AbstractCall& call_instr,
    FrameState& pre_call_state,
    size_t num_pos) {
  // When defaults fill missing positionals there are no surplus
  // arguments; the tuple is empty.
  BorrowedRef<PyCodeObject> code{call_instr.func->func_code};
  const size_t co_argcount = code->co_argcount;
  // Positional values beyond co_argcount are surplus; when defaults fill
  // missing positionals there are none and the tuple is empty.
  const size_t num_extra = num_pos < co_argcount ? 0 : num_pos - co_argcount;
  auto varargs_reg = caller.env.allocateRegister();
  if (num_extra == 0) {
    auto make_tuple =
        LoadConst::create(varargs_reg, Type::fromObject(PyTuple_New(0)));
    make_tuple->insertBefore(*call_instr.instr);
    return varargs_reg;
  }

  varargs_reg->setType(TMortalTupleExact);
  // In the callee frame state, the tuple is created before the callee frame
  // is pushed on the stack.
  auto make_tuple = MakeTuple::create(varargs_reg, num_extra, pre_call_state);
  make_tuple->insertBefore(*call_instr.instr);

  auto fill = InitTupleElements::create(num_extra + 1);
  fill->setOperand(0, varargs_reg);
  for (size_t i = 0; i < num_extra; ++i) {
    fill->setOperand(i + 1, call_instr.arg(co_argcount + i));
  }
  fill->insertBefore(*call_instr.instr);

  // MakeTuple/InitTupleElements are not replayable, so they hide any
  // earlier snapshot from later guards in this block (bindGuards binds
  // each guard to its dominating same-block snapshot). Restore coverage
  // with a fresh snapshot, as the builder does before deopting
  // instructions.
  auto post_snapshot = Snapshot::create(pre_call_state);
  post_snapshot->insertBefore(*call_instr.instr);
  return varargs_reg;
}

void emitDefaultsGuard(
    Function& caller,
    const AbstractCall& call_instr,
    FrameState& pre_call_state,
    BorrowedRef<PyTupleObject> defaults) {
  auto snapshot = Snapshot::create(pre_call_state);
  snapshot->insertBefore(*call_instr.instr);
  Register* func_reg;
  if (call_instr.target != nullptr) {
    func_reg = call_instr.target;
  } else {
    func_reg = caller.env.allocateRegister();
    auto func_type = Type::fromObject(caller.env.addReference(call_instr.func));
    func_reg->setType(func_type);
    auto func_const = LoadConst::create(func_reg, func_type);
    func_const->insertBefore(*call_instr.instr);
  }
  Register* defaults_obj = caller.env.allocateRegister();
  JIT_THROW_IF(defaults == nullptr, "should have defaults for unused args");
  auto load_defaults = LoadField::create(
      defaults_obj,
      func_reg,
      "func_defaults",
      offsetof(PyFunctionObject, func_defaults),
      TTuple);
  caller.env.addReference(defaults);
  load_defaults->insertBefore(*call_instr.instr);
  Register* guarded = caller.env.allocateRegister();
  auto guard = GuardIs::create(guarded, defaults, defaults_obj);
  guard->setFrameState(pre_call_state);
  guard->insertBefore(*call_instr.instr);
}

std::vector<Register*> resolveArgs(
    Function& caller,
    const AbstractCall& call_instr,
    FrameState& pre_call_state,
    size_t co_argcount,
    const MappedCallArgs& mapping,
    BorrowedRef<PyTupleObject> defaults) {
  // Fix up argument mismatches at the call site, before splicing in the
  // callee. Missing positional arguments are filled from the callee's
  // func_defaults, guarded so that reassigning __defaults__ deopts,
  // mirroring resolveArgs in simplify.cpp; surplus arguments to a *args
  // callee are packed into the varargs tuple. This lives here (rather than
  // in Simplify) so that calls inside freshly inlined bodies, which haven't
  // been simplified yet, can still be inlined. It must stay at the call
  // site (rather than moving into the callee entry block) so that the
  // deopting instructions' FrameState depth matches their inline depth.

  // canInline() validated the mapping above; reuse it here to materialize
  // the fixup. Provided parameters (positional or keyword) map to call
  // operands; other positional parameters are filled from func_defaults
  // (only possible without kwargs, which require every argument to be
  // provided).
  const size_t num_defaults =
      defaults == nullptr ? 0 : static_cast<size_t>(PyTuple_GET_SIZE(defaults));
  // Leading defaults beyond co_argcount are ignored by CPython; the rest
  // right-align against the positional parameters.
  const size_t num_non_defaults =
      num_defaults > co_argcount ? 0 : co_argcount - num_defaults;
  const size_t default_offset =
      num_defaults > co_argcount ? num_defaults - co_argcount : 0;

  // Resolved register for each positional then kwonly parameter: the call
  // operand when provided, otherwise a constant materialized below.
  std::vector<Register*> param_regs = mapping.param_regs;
  bool defaults_guard_emitted = false;
  for (size_t i = 0; i < co_argcount; ++i) {
    if (param_regs.at(i) != nullptr) {
      continue;
    }
    if (!defaults_guard_emitted) {
      // First missing argument: guard that func_defaults is unchanged.
      // The snapshot gives bindGuards a same-block FrameState for the
      // guard; on failure we deopt to the caller at the call site.
      emitDefaultsGuard(caller, call_instr, pre_call_state, defaults);
    }
    const size_t default_idx = i - num_non_defaults + default_offset;
    JIT_THROW_IF(
        default_idx >= PyTuple_GET_SIZE(defaults), "out of range defaults");
    auto def = PyTuple_GET_ITEM(defaults, default_idx);
    JIT_THROW_IF(def == nullptr, "expected non-null default");
    auto type = Type::fromObject(caller.env.addReference(def));
    Register* def_reg = caller.env.allocateRegister();
    def_reg->setType(type);
    auto load_const = LoadConst::create(def_reg, type);
    load_const->insertBefore(*call_instr.instr);
    param_regs[i] = def_reg;
  }
  return param_regs;
}

// Attempt to inline a single call.  On success returns the spliced-in callee
// region (entry/exit blocks) so the caller can re-scan it for nested calls; on
// failure returns nullopt (the reason is logged into the caller's stats).
std::optional<InlineResult> inlineFunctionCall(
    Function& caller,
    const AbstractCall& call_instr) {
  BorrowedRef<PyFunctionObject> callee = call_instr.func;

  // We are only able to inline functions that were already preloaded, since we
  // can't safely preload anything mid-compile (preloading can execute arbitrary
  // Python code and raise Python exceptions). Currently this means that in
  // single-function-compile mode we are limited to inlining functions loaded as
  // globals, or statically invoked. See `preloadFuncAndDeps` for what
  // dependencies we will preload. In batch-compile mode we can inline anything
  // that is part of the batch.
  // The callee preloader is also our race-free source of func_defaults
  // below: captured with the GIL held and kept alive, while the function
  // object itself cannot be safely touched on a background compile without
  // the GIL.
  Preloader* preloader = preloaderManager().find(callee);
  if (!preloader) {
    logInlineFailure(caller, callee, InlineFailureType::kNeedsPreload);
    return std::nullopt;
  }

  auto mapping = canInline(caller, call_instr, preloader->funcDefaults());
  if (!mapping) {
    return std::nullopt;
  }

  auto caller_frame_state =
      std::make_unique<FrameState>(*call_instr.instr->frameState());

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

  // Keep the callee and transitively its code object, which a
  // PyFunctionObject owns strongly alive
  caller.env.addReference(call_instr.func);

  // This logging is parsed by jitlist_bisect.py to find inlined functions.
  JIT_LOGIF(
      getConfig().log.debug_inliner || getConfig().log.debug,
      "Inlining function {} into {}",
      callee_name,
      caller.fullname);

  BorrowedRef<PyCodeObject> callee_code = preloader->code();

  const bool has_varargs = callee_code->co_flags & CO_VARARGS;
  const size_t co_argcount = static_cast<size_t>(callee_code->co_argcount);
  const size_t co_kwonly = static_cast<size_t>(callee_code->co_kwonlyargcount);
  const size_t starargs_idx = co_argcount + co_kwonly;

  // FrameState to attach to the fixup instructions below. The call
  // instruction's own FrameState reflects the stack after its operands were
  // popped -- a state that never exists in the interpreter -- so it cannot
  // be used to resume the call. Rebuild the pre-call state (operands back
  // on the stack, in order); deopting with it re-executes the call in the
  // interpreter.
  FrameState pre_call_state(*call_instr.instr->frameState());
  pre_call_state.stack.clear();
  for (size_t i = 0, n = call_instr.instr->numOperands(); i < n; ++i) {
    pre_call_state.stack.push(call_instr.instr->getOperand(i));
  }

  // canInline() validated the mapping above; resolveArgs() materializes the
  // fixup from it (provided parameters map to call operands, the rest are
  // filled from func_defaults).
  auto resolved_args = resolveArgs(
      caller,
      call_instr,
      pre_call_state,
      co_argcount,
      *mapping,
      preloader->funcDefaults());
  const size_t num_pos = mapping->num_pos;

  // Register holding the packed varargs tuple, if the callee takes *args.
  Register* varargs_reg = nullptr;
  if (has_varargs) {
    varargs_reg = populateVarArgs(caller, call_instr, pre_call_state, num_pos);
  }

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
    Register* code_obj = caller.env.allocateRegister();
    auto load_code = LoadField::create(
        code_obj,
        call_instr.target,
        "func_code",
        offsetof(PyFunctionObject, func_code),
        TObject);
    Register* guarded_code = caller.env.allocateRegister();
    auto guard_code = GuardIs::create(guarded_code, callee_code, code_obj);
    call_instr.instr->expandInto(
        {load_code, guard_code, begin_inlined_function, callee_branch});
  } else {
    call_instr.instr->expandInto({begin_inlined_function, callee_branch});
  }
  tail->push_front(EndInlinedFunction::create(begin_inlined_function));

  // Transform LoadArg into Assign.  They'll only be in the entry block.
  for (auto it = result.entry->begin(); it != result.entry->end();) {
    auto& instr = *it;
    ++it;

    if (!instr.isLoadArg()) {
      continue;
    }
    auto load_arg = static_cast<LoadArg*>(&instr);
    Register* src = (has_varargs && load_arg->argIdx() == starargs_idx)
        ? varargs_reg
        : resolved_args.at(load_arg->argIdx());
    auto assign = Assign::create(instr.output(), src);
    instr.replaceWith(*assign);
    delete &instr;
  }

  // Transform Return into Assign+Branch.  The HIRBuilder guarantees that the
  // callee's exit block always has a Return, even if it is unreachable.
  auto return_instr = result.exit->getTerminator();
  JIT_CHECK(
      return_instr->isReturn(),
      "terminator from inlined function should be Return");
  auto assign =
      Assign::create(call_instr.instr->output(), return_instr->getOperand(0));
  auto return_branch = Branch::create(tail);
  return_instr->expandInto({assign, return_branch});
  delete return_instr;

  delete call_instr.instr;
  caller.inline_function_stats.num_inlined_functions++;
  return result;
}

// Validate a dynamic call's function target and, if it names a concrete
// function we can inline, append it as a candidate.  `target` is the register
// holding the callee, `nargs` the number of call operands excluding the
// callee itself (for KwArgs calls this includes the keyword values plus the
// trailing kwnames tuple; see mapCallArgs()).
void maybeAddDynamicCall(
    Function& irfunc,
    DeoptBase* instr,
    Register* target,
    size_t nargs,
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
  // KwArgs calls are supported when every argument is provided; see
  // mapCallArgs(). kwargs mapping is validated in canInline().
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
    if (instr.isVectorCall()) {
      auto call = static_cast<VectorCall*>(&instr);
      maybeAddDynamicCall(irfunc, call, call->func(), call->numArgs(), calls);
    } else if (instr.isCallMethod()) {
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
        maybeAddDynamicCall(irfunc, call, target, call->numArgs(), calls);
      }
    } else if (instr.isInvokeStaticFunction()) {
      auto call = static_cast<InvokeStaticFunction*>(&instr);
      calls.emplace_back(call->func(), call->numArgs() - 1, call);
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
    Instr* terminator = block->getTerminator();
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
    if (it->isSnapshot()) {
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

void InlineFunctionCalls::run(Function& irfunc) {
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

  const size_t cost_limit = getConfig().inliner.cost_limit;
  const size_t depth_limit = getConfig().inliner.depth_limit;
  const size_t cold_threshold = getConfig().inliner.cold_call_threshold;

  const size_t original_cost = codeCost(irfunc.code);
  size_t cost = original_cost;

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

  // Inlining spliced callee sub-CFGs into the caller, changing block structure,
  // so drop any cached dominance before CleanCFG (which consults it).
  if (cost != original_cost) {
    irfunc.invalidateDomTree();
  }

  // Inlining a callee with no reachable return leaves unreachable blocks
  // behind.  We can't drop them inside the loop above (that might free call
  // instructions still queued for inlining), so we clean up here once the loop
  // is done.  CleanCFG removes the unreachable blocks; CopyPropagation first
  // collapses the Assigns the inliner introduced.
  CopyPropagation{}.run(irfunc);
  CleanCFG{}.run(irfunc);
}

void BeginInlinedFunctionElimination::run(Function& irfunc) {
  std::vector<EndInlinedFunction*> ends;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.isEndInlinedFunction()) {
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
