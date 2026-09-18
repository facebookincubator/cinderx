// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/func_deopt_patcher.h"

namespace cinderx::jit {

FuncCodeDeoptPatcher::FuncCodeDeoptPatcher(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyCodeObject> expected_code)
    : func_{func}, expected_code_{expected_code} {}

bool FuncCodeDeoptPatcher::assumptionsStillValid() const {
  return func_->func_code == reinterpret_cast<PyObject*>(expected_code_.get());
}

BorrowedRef<PyFunctionObject> FuncCodeDeoptPatcher::func() const {
  return func_;
}

} // namespace cinderx::jit
