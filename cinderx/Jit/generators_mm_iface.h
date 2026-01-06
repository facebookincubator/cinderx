// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

namespace jit {

struct JitGenObject;

class IJitGenFreeList {
 public:
  IJitGenFreeList() = default;
  virtual ~IJitGenFreeList() = default;

  virtual std::pair<JitGenObject*, size_t> allocate(
      BorrowedRef<PyCodeObject> code,
      uint64_t jit_spill_words) = 0;
  virtual void free(PyObject* ptr) = 0;
};

} // namespace jit
