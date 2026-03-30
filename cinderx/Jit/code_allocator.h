// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/code_allocator_iface.h"

#include <asmjit/asmjit.h>

#include <atomic>
#include <mutex>
#include <span>
#include <vector>

namespace jit {

/*
  A CodeAllocator allocates memory for live JIT code. This is an abstract
  interface for now to allow us to easily switch between implementations based
  on an AsmJIT "Runtime", or an implemenation which uses huge pages.

  For now we only support one global per-process CodeAllocator, accessible via
  ::get(). This is primarily to maximize the efficiency when using huge pages
  by avoiding independent huge-page pools which are all a little under-utilized.

  We may one day need non-global code allocators if we want to do fancy things
  like accommodate memory pools with different allocation characteristics, or
  have multiple threads which might compile independently.
*/
class CodeAllocator : public ICodeAllocator {
 public:
  ~CodeAllocator() override = default;

  // To be called once by JIT initialization after enough configuration has been
  // loaded to determine which global code allocator type to use.
  static ICodeAllocator* make();

  AllocateResult addCode(asmjit::CodeHolder* code) override;
  asmjit::Error releaseCode(void* code) override;
  bool contains(const void* ptr) const override;
  size_t usedBytes() const override;
  const asmjit::Environment& asmJitEnvironment() const override;

 protected:
  asmjit::JitRuntime runtime_;
  std::atomic<size_t> used_bytes_{0};
};

// A code allocator which tries to allocate all code on huge pages.
//
// When multiple code sections are enabled, hot code is allocated on huge pages
// and cold code is allocated on separate pages (optionally huge pages as well,
// controlled by the cold_code_huge_pages config).
class CodeAllocatorCinder : public CodeAllocator {
 public:
  ~CodeAllocatorCinder() override;

  size_t lostBytes() const {
    return lost_bytes_.load(std::memory_order_relaxed);
  }

  size_t fragmentedAllocs() const {
    return fragmented_allocs_.load(std::memory_order_relaxed);
  }

  size_t hugeAllocs() const {
    return huge_allocs_.load(std::memory_order_relaxed);
  }

  AllocateResult addCode(asmjit::CodeHolder* code) override;
  asmjit::Error releaseCode(void* code) override;
  bool contains(const void* ptr) const override;

 private:
  // Add code with hot/cold section splitting. Called by addCode() when
  // multiple_code_sections is enabled. Caller must hold allocator_mutex_.
  AllocateResult addSplitCode(asmjit::CodeHolder* code);

  // Ensure the given bump allocator has at least `size` bytes free, allocating
  // a new chunk if necessary.
  void ensureSpace(
      uint8_t*& alloc,
      size_t& alloc_free,
      size_t size,
      bool use_huge_pages);

  // Protects all allocator-owned state used by addCode()/contains().
  mutable std::mutex allocator_mutex_;

  // List of all chunks allocated, for use in deallocation and contains().
  std::vector<std::span<uint8_t>> allocations_;

  // Hot code allocation state.
  uint8_t* hot_alloc_{nullptr};
  size_t hot_alloc_free_{0};

  // Cold code allocation state (used when multiple code sections are enabled).
  uint8_t* cold_alloc_{nullptr};
  size_t cold_alloc_free_{0};

  // Number of bytes in total lost when allocations didn't fit neatly into
  // the bytes remaining in a chunk so a new one was allocated.
  std::atomic<size_t> lost_bytes_{0};
  // Number of chunks allocated which successfully used huge pages.
  std::atomic<size_t> huge_allocs_{0};
  // Number of chunks allocated which did not use huge pages.
  std::atomic<size_t> fragmented_allocs_{0};
};

void populateCodeSections(
    std::vector<std::pair<void*, std::size_t>>& output_vector,
    asmjit::CodeHolder& code,
    void* entry);

} // namespace jit
