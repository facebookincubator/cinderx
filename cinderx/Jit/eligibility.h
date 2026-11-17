// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/context_iface.h"

namespace cinderx {
struct ModuleState;
}

namespace cinderx::jit {

bool hasRequiredCodeFlags(BorrowedRef<PyCodeObject> code);

JitEligibility getCompilationEligibility(
    ModuleState& state,
    BorrowedRef<PyFunctionObject> func);

JitEligibility getCompilationEligibility(
    ModuleState& state,
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code);

} // namespace cinderx::jit
