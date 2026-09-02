// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/eligibility.h"

#include "cinderx/module_state.h"

#include <string_view>

namespace cinderx::jit {

namespace {

constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;

bool isCinderModule(BorrowedRef<> module_name) {
  if (module_name == nullptr || !PyUnicode_Check(module_name)) {
    return false;
  }
  std::string_view name = PyUnicode_AsUTF8(module_name);
  return name == "cinderx";
}

} // namespace

bool hasRequiredCodeFlags(BorrowedRef<PyCodeObject> code) {
  return (code->co_flags & kRequiredCodeFlags) == kRequiredCodeFlags;
}

JitEligibility getCompilationEligibility(BorrowedRef<PyFunctionObject> func) {
  auto* state = cinderx::getModuleState();
  if (state == nullptr || state->jit_context == nullptr ||
      isCinderModule(func->func_module)) {
    return JitEligibility::Ineligible;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (!hasRequiredCodeFlags(code)) {
    return JitEligibility::Ineligible;
  }

  if (auto* jit_list = state->jit_list.get()) {
    if (jit_list->lookupFunc(func) == 1) {
      return JitEligibility::JitListEligible;
    }
    return JitEligibility::Ineligible;
  }

  return JitEligibility::Eligible;
}

JitEligibility getCompilationEligibility(
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code) {
  auto* state = cinderx::getModuleState();
  if (state == nullptr || state->jit_context == nullptr ||
      isCinderModule(module_name) || !hasRequiredCodeFlags(code)) {
    return JitEligibility::Ineligible;
  }

  if (auto* jit_list = state->jit_list.get()) {
    if (jit_list->lookupCode(code) == 1 ||
        jit_list->lookupName(module_name, code->co_qualname) == 1) {
      return JitEligibility::JitListEligible;
    }
    return JitEligibility::Ineligible;
  }

  return JitEligibility::Eligible;
}

} // namespace cinderx::jit
