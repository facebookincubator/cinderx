// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <span>

namespace jit {

#ifndef WIN32
class MmapFile {
 public:
  constexpr MmapFile() = default;
  ~MmapFile();

  void open(const char* filename);
  void close();

  bool isOpen() const;

  std::span<const std::byte> data();

 private:
  const std::byte* data_{nullptr};
  size_t size_{0};
};

#endif

} // namespace jit
