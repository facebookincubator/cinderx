// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/nested_compile.h"

#include "cinderx/Common/code.h"

namespace cinderx::jit {

void publishNestedCompileData(NestedCompileData* data) {
  // Some code objects can't carry extra data at all, e.g. the read-only ones
  // in 3.14+.  They just miss out on the fast path.
  if (CodeExtra* extra = codeExtra(data->code())) {
    Ci_code_extra_set_nested_compile_data(extra, data);
  }
}

void unpublishNestedCompileData(NestedCompileData* data) {
  if (CodeExtra* extra = codeExtraIfPresent(data->code())) {
    Ci_code_extra_clear_nested_compile_data(extra, data);
  }
}

NestedCompileData* nestedCompileData(BorrowedRef<PyCodeObject> code) {
  CodeExtra* extra = codeExtraIfPresent(code);
  return extra == nullptr ? nullptr
                          : static_cast<NestedCompileData*>(
                                Ci_code_extra_get_nested_compile_data(extra));
}

} // namespace cinderx::jit
