// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

namespace cinderx {
class IAsyncLazyValueState {
 public:
  virtual ~IAsyncLazyValueState() = default;

  virtual bool init() = 0;
  virtual BorrowedRef<PyTypeObject> asyncLazyValueType() = 0;
};
} // namespace cinderx
