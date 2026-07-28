// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/long.h"

#include "cinderx/Common/log.h"

extern "C" {

#include "internal/pycore_long.h"

} // extern "C"

namespace cinderx {

BorrowedRef<PyLongObject> smallInt(int32_t n) {
  JIT_THROW_IF(
      n < -_PY_NSMALLNEGINTS || n >= _PY_NSMALLPOSINTS,
      "{} is out of bounds for small Python longs ([{}, {}])",
      n,
      -_PY_NSMALLNEGINTS,
      _PY_NSMALLPOSINTS - 1);
  return &_PyLong_SMALL_INTS[n + _PY_NSMALLNEGINTS];
}

} // namespace cinderx
