// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_runtime.h"

namespace jit {

const int64_t CodeRuntime::kPyCodeOffset =
    RuntimeFrameState::codeOffset() + CodeRuntime::frameStateOffset();

void CodeRuntime::releaseReferences() {
  references_.clear();
}

void CodeRuntime::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

} // namespace jit
