// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace cinderx::jit {

class alignas(16) CodeRuntime;

enum class JitEligibility { Ineligible, JitListEligible, Eligible };

class IJitContext {
 public:
  IJitContext() {}
  virtual ~IJitContext() = default;

  virtual CodeRuntime* lookupCodeRuntime(
      BorrowedRef<PyFunctionObject> func) = 0;
};

} // namespace cinderx::jit
