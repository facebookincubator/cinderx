// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <optional>
#include <string_view>

namespace jit {

class ISymbolizer {
 public:
  ISymbolizer() = default;
  virtual ~ISymbolizer() = default;

  // Return a string view whose lifetime is tied to the Symbolizer lifetime on
  // success. On failure, return std::nullopt.
  virtual std::optional<std::string_view> symbolize(const void* func) = 0;
};

} // namespace jit
