// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

namespace jit {

class IRuntime {
 public:
  IRuntime() {}
  virtual ~IRuntime() = default;

  virtual BorrowedRef<> zero() = 0;
};

} // namespace jit
