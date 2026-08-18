// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/rewrite.h"

namespace cinderx::jit::lir {

// Rewrites after register allocation
class PostRegAllocRewrite : public Rewrite {
 public:
  PostRegAllocRewrite(Function* func, codegen::Environ* env)
      : Rewrite(func, env) {
    registerRewrites();
  }

 private:
  void registerRewrites();
};

#if defined(CINDER_AARCH64)

// Peephole rewrites run once the instruction stream is otherwise final.
void runPostRegAllocPeephole(Function* func);

#endif

} // namespace cinderx::jit::lir
