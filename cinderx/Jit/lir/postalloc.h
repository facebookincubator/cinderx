// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/rewrite.h"

namespace jit::lir {

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

} // namespace jit::lir
