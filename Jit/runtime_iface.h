// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

namespace jit {

class IRuntime {
 public:
  IRuntime() {}
  virtual ~IRuntime() = default;
};

} // namespace jit
