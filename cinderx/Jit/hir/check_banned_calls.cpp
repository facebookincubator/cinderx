// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/check_banned_calls.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/context.h"

#include <optional>
#include <string>
#include <string_view>

namespace cinderx::jit::hir {

namespace {

// exec(source, globals, locals) and eval(source, globals, locals) only read the
// calling frame when globals or locals are omitted.
constexpr std::size_t kMinSafeArgs = 3;

bool isBannedBuiltinName(std::string_view name) {
  return name == "exec" || name == "eval";
}

// If `callable` is known to be the builtin exec() or eval(), return its name.
std::optional<std::string> bannedBuiltinName(Register* callable) {
  Type type = callable->type();
  if (type.hasObjectSpec()) {
    PyObject* obj = type.objectSpec();
    if (!PyCFunction_Check(obj)) {
      return std::nullopt;
    }
    auto name = getContext()->builtins().find(
        reinterpret_cast<PyCFunctionObject*>(obj)->m_ml);
    if (name.has_value() && isBannedBuiltinName(*name)) {
      return name;
    }
    return std::nullopt;
  }

  // Globals that can't be cached are loaded with no type information, so fall
  // back to checking the name being loaded.
  Instr* def = callable->instr();
  if (def != nullptr && def->isLoadGlobal()) {
    std::string name = unicodeAsString(static_cast<LoadGlobal*>(def)->name());
    if (isBannedBuiltinName(name)) {
      return name;
    }
  }
  return std::nullopt;
}

// Number of arguments passed to the callee, excluding the kwnames tuple.
std::size_t numCallArgs(std::size_t num_operand_args, CallFlags flags) {
  return (flags & CallFlags::KwArgs) ? num_operand_args - 1 : num_operand_args;
}

void checkCall(const Function& func, Register* callable, std::size_t nargs) {
  if (nargs >= kMinSafeArgs) {
    return;
  }
  if (auto name = bannedBuiltinName(callable)) {
    JIT_THROW(
        "Cannot compile {} to HIR because it calls '{}' without explicit "
        "globals and locals",
        func.fullname,
        *name);
  }
}

void checkCallMethod(const Function& func, const CallMethod& call) {
  std::size_t nargs = numCallArgs(call.numArgs(), call.flags());
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    // Operands are [callable, self_or_null, args...].  Only count self when
    // it's known to be passed.
    Register* self = call.getOperand(1);
    checkCall(
        func,
        call.getOperand(0),
        nargs + (self->type().couldBe(TNullptr) ? 0 : 1));
  } else {
    // Operands are [method_or_null, callable_or_self, args...].  When the
    // first operand is the callable, the second is passed as self.
    checkCall(func, call.getOperand(0), nargs + 1);
    checkCall(func, call.getOperand(1), nargs);
  }
}

void checkInstr(const Function& func, const Instr& instr) {
  if (instr.isVectorCall()) {
    auto& call = static_cast<const VectorCall&>(instr);
    checkCall(func, call.func(), numCallArgs(call.numArgs(), call.flags()));
  } else if (instr.isCallMethod()) {
    checkCallMethod(func, static_cast<const CallMethod&>(instr));
  } else if (instr.isCallEx()) {
    // The argument count isn't known statically, so assume the worst.
    checkCall(func, static_cast<const CallEx&>(instr).func(), 0);
  }
}

} // namespace

bool CheckBannedCalls::run(Function& func) {
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      checkInstr(func, instr);
    }
  }
  return false;
}

} // namespace cinderx::jit::hir
