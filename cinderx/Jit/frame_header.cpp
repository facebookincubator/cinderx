// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame_header.h"

#include <cinderx/Common/code.h>
#include <cinderx/Common/util.h>
#include <cinderx/Jit/config.h>

#include <unordered_set>
#include <vector>

namespace jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

  if (getConfig().frame_mode == FrameMode::kLightweight) {
    return sizeof(FrameHeader) + sizeof(PyObject*) * code->co_framesize;
  }

  return 0;
}

} // namespace jit
