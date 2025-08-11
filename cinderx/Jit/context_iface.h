// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace jit {
class IJitContext {
 public:
  IJitContext() {}
  virtual ~IJitContext() = default;
};

} // namespace jit
