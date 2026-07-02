// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <string>
#include <string_view>

namespace cinderx::jit::hir {

class HIRStats final : public Pass {
 public:
  HIRStats() : Pass("HIRStats") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<HIRStats> factory() {
    return std::make_unique<HIRStats>();
  }

  void dump(std::string_view function_name) const {
    stats_.dump(function_name);
  }

 private:
  struct Stats {
    UnorderedMap<std::string, int> instrs;
    UnorderedMap<std::string, int> output_types;

    void dump(std::string_view function_name) const;
  };

  Stats stats_;
};

} // namespace cinderx::jit::hir
