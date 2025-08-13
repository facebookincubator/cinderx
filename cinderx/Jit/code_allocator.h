// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/code_allocator_iface.h"
#include "cinderx/Jit/codegen/code_section.h"

#include <asmjit/asmjit.h>

#include <atomic>
#include <span>
#include <unordered_map>
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
class CodeAllocatorCinder : public CodeAllocator {
 public:
  ~CodeAllocatorCinder() override;

  size_t lostBytes() const {
    return lost_bytes_;
  }

  size_t fragmentedAllocs() const {
    return fragmented_allocs_;
  }

  size_t hugeAllocs() const {
    return huge_allocs_;
  }

  AllocateResult addCode(asmjit::CodeHolder* code) override;
  asmjit::Error releaseCode(void* code) override;
  bool contains(const void* ptr) const override;

 private:
  // List of chunks allocated for use in deallocation
  std::vector<std::span<uint8_t>> allocations_;

  // Pointer to next free address in the current chunk
  uint8_t* current_alloc_{nullptr};
  // Free space in the current chunk
  size_t current_alloc_free_{0};

  // Number of bytes in total lost when allocations didn't fit neatly into
  // the bytes remaining in a chunk so a new one was allocated.
  size_t lost_bytes_{0};
  // Number of chunks allocated (= to number of huge pages used)
  size_t huge_allocs_{0};
  // Number of chunks allocated which did not use huge pages.
  size_t fragmented_allocs_{0};
};

class MultipleSectionCodeAllocator : public CodeAllocator {
 public:
  ~MultipleSectionCodeAllocator() override;

  AllocateResult addCode(asmjit::CodeHolder* code) override;
  asmjit::Error releaseCode(void* code) override;
  bool contains(const void* ptr) const override;

 private:
  void createSlabs() noexcept;

  std::unordered_map<codegen::CodeSection, uint8_t*> code_sections_;
  std::unordered_map<codegen::CodeSection, size_t> code_section_free_sizes_;

  uint8_t* code_alloc_{nullptr};
  size_t total_allocation_size_{0};
};

void populateCodeSections(
    std::vector<std::pair<void*, std::size_t>>& output_vector,
    asmjit::CodeHolder& code,
    void* entry);

} // namespace jit
