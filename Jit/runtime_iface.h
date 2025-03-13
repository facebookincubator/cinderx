// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#include "cinderx/Common/ref.h"

namespace jit {

class IRuntime {
 public:
  IRuntime() {}
  virtual ~IRuntime() = default;

  virtual BorrowedRef<> zero() = 0;
};

} // namespace jit
