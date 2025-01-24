// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/rewrite.h"

namespace jit::lir {

// Rewrites after LIR generation
class PostGenerationRewrite : public Rewrite {
 public:
  PostGenerationRewrite(Function* func, codegen::Environ* env)
      : Rewrite(func, env) {
    registerRewrites();
  }

 private:
  void registerRewrites();
};

} // namespace jit::lir
