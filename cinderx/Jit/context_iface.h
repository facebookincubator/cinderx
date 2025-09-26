// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace jit {

class alignas(16) CodeRuntime;

class IJitContext {
 public:
  IJitContext() {}
  virtual ~IJitContext() = default;

  virtual CodeRuntime* lookupCodeRuntime(
      BorrowedRef<PyFunctionObject> func) = 0;
};

} // namespace jit
