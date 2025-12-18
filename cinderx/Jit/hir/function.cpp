// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/function.h"

namespace jit::hir {

// Be intentional about HIR structure sizes.  There's no hard limit on what
// these sizes have to be, but we should be aware when we change them.
//
// Ignore it for libc++ for now though, too tricky to track multiple
// implementations.
#ifndef _LIBCPP_VERSION
static_assert(sizeof(Function) == 48 * kPointerSize);
static_assert(sizeof(CFG) == 5 * kPointerSize);
static_assert(sizeof(BasicBlock) == 20 * kPointerSize);
static_assert(sizeof(Instr) == 6 * kPointerSize);
#endif

Function::Function() {}

Function::~Function() {
  // Serialize as we alter ref-counts on potentially global objects.
  ThreadedCompileSerialize guard;
  code.reset();
  builtins.reset();
  globals.reset();
  prim_args_info.reset();
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

bool Function::returnsPrimitive() const {
  return return_type <= TPrimitive;
}

bool Function::returnsPrimitiveDouble() const {
  return return_type <= TCDouble;
}

void Function::setCompilationPhaseTimer(
    std::unique_ptr<CompilationPhaseTimer> cpt) {
  compilation_phase_timer = std::move(cpt);
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

BorrowedRef<PyCodeObject> Function::codeFor(const Instr& instr) const {
  if (instr.IsBeginInlinedFunction()) {
    auto bif = static_cast<const BeginInlinedFunction*>(&instr);
    return bif->func()->func_code;
  }
  if (instr.IsLoadGlobalCached()) {
    auto load_global = static_cast<const LoadGlobalCached*>(&instr);
    return load_global->code();
  }
  if (auto deopt_base = instr.asDeoptBase()) {
    auto fs = deopt_base->frameState();
    return fs != nullptr ? fs->code : nullptr;
  }
  const FrameState* fs = instr.getDominatingFrameState();
  return fs == nullptr ? code : fs->code;
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

} // namespace jit::hir
