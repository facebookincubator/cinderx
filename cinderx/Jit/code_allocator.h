// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/jit_unwind.h"

#include <asmjit/asmjit.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace cinderx::jit {

// Resulting address and status code after calling CodeAllocator::addCode().
struct AllocateResult {
  void* addr{nullptr};
  asmjit::Error error{asmjit::kErrorNotInitialized};
};

/*
  A CodeAllocator allocates memory for live JIT code. It is a polymorphic base
  so we can switch between the default AsmJIT runtime and an implementation
  which uses huge pages.

  For now we only support one global per-process CodeAllocator, accessible via
  ::get(). This is primarily to maximize the efficiency when using huge pages
  by avoiding independent huge-page pools which are all a little under-utilized.

  We may one day need non-global code allocators if we want to do fancy things
  like accommodate memory pools with different allocation characteristics, or
  have multiple threads which might compile independently.
*/
class CodeAllocator {
 public:
  CodeAllocator();
  // Unregisters any unwind info still held for code that was never
  // released, before the asmjit runtime is torn down.
  virtual ~CodeAllocator();

  CodeAllocator(const CodeAllocator&) = delete;
  CodeAllocator& operator=(const CodeAllocator&) = delete;
  CodeAllocator(CodeAllocator&&) = delete;
  CodeAllocator& operator=(CodeAllocator&&) = delete;

  // Number of unwind-info registrations held for code allocated through
  // `addCode` and not yet released.  Empty for `CodeAllocatorCinder`,
  // which registers once per chunk instead (see
  // `CodeAllocatorCinder::chunkUnwindRegistrationCount`).
  size_t unwindRegistrationCount() const {
    std::lock_guard lock{runtime_mutex_};
    size_t count = 0;
    for (const auto& [addr, registrations] : unwind_registrations_) {
      count += registrations.size();
    }
    return count;
  }

  // To be called once by JIT initialization after enough configuration has been
  // loaded to determine which global code allocator type to use.
  [[nodiscard]] static CodeAllocator* make();

  virtual AllocateResult addCode(asmjit::CodeHolder* code);
  virtual asmjit::Error releaseCode(void* code);
  virtual bool contains(const void* ptr) const;
  virtual size_t usedBytes() const;
  virtual const asmjit::Environment& asmJitEnvironment() const;

  virtual void atForkPrepare();
  virtual void atForkParent();
  virtual void atForkChild();

 protected:
  // Register unwind info for every executable section of `code` at
  // `addr`, and record the registrations under that address so
  // `releaseCode` can deregister them before the memory is freed.
  // Caller must hold `runtime_mutex_`.  Registration failures are
  // non-fatal: sections that could not be registered get no entry.
  void registerUnwindInfo(asmjit::CodeHolder* code, void* addr);

  asmjit::JitRuntime runtime_;
  std::atomic<size_t> used_bytes_{0};

  // Per-function unwind-info registrations, keyed by the address
  // `addCode` returned.  Guarded by `runtime_mutex_`, which is always
  // taken before any unwinder lock, matching the order in `addCode`.
  std::unordered_map<void*, std::vector<std::unique_ptr<JitUnwindRegistration>>>
      unwind_registrations_;

  // Serializes every operation that reaches asmjit's own JitAllocator lock.
  // That lock is private to asmjit with no way to reset it in a forked child,
  // so this makes it observably free at fork time instead: atForkPrepare()
  // holds this, which means no thread can be inside asmjit when the fork
  // happens.
  mutable std::mutex runtime_mutex_;
};

// A code allocator which tries to allocate all code on huge pages.
//
// When multiple code sections are enabled, hot code is allocated on huge pages
// and cold code is allocated on separate pages (optionally huge pages as well,
// controlled by the cold_code_huge_pages config).
class CodeAllocatorCinder : public CodeAllocator {
 public:
  CodeAllocatorCinder() = default;
  ~CodeAllocatorCinder() override;

  CodeAllocatorCinder(const CodeAllocatorCinder&) = delete;
  CodeAllocatorCinder& operator=(const CodeAllocatorCinder&) = delete;
  CodeAllocatorCinder(CodeAllocatorCinder&&) = delete;
  CodeAllocatorCinder& operator=(CodeAllocatorCinder&&) = delete;

  size_t lostBytes() const {
    return lost_bytes_.load(std::memory_order_relaxed);
  }

  size_t fragmentedAllocs() const {
    return fragmented_allocs_.load(std::memory_order_relaxed);
  }

  size_t hugeAllocs() const {
    return huge_allocs_.load(std::memory_order_relaxed);
  }

  // Number of unwind-info registrations held for allocated chunks: one
  // per chunk, shared by every function and trampoline in the chunk.
  // Caller must hold `allocator_mutex_`.
  size_t chunkUnwindRegistrationCount() const {
    return chunk_unwind_registrations_.size();
  }

  AllocateResult addCode(asmjit::CodeHolder* code) override;
  asmjit::Error releaseCode(void* code) override;
  bool contains(const void* ptr) const override;

  void atForkPrepare() override;
  void atForkParent() override;
  void atForkChild() override;

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

  // Ensure both hot and cold bump allocators have enough space for the given
  // sizes. If either needs a new allocation, a single contiguous region is
  // allocated and split between hot (first half) and cold (second half). This
  // guarantees cross-section jumps are within ARM64's relative branch range
  // (±128MB for B/BL, ±1MB for B.cond). Used only on aarch64.
  void ensureSplitSpace(size_t hot_needed, size_t cold_needed);

  // Register unwind info for a whole chunk.  Chunks are never freed
  // or reused while the allocator lives, so one registration covers
  // every function and trampoline allocated from the chunk until the
  // allocator is destroyed.  No other registration may overlap the
  // chunk (see `jit_unwind.h`).  Caller must hold `allocator_mutex_`.
  // A failed registration is non-fatal and records nothing.
  void registerChunkUnwindInfo(void* chunk_addr, size_t chunk_size);

  // Protects all allocator-owned state used by addCode()/contains().
  mutable std::mutex allocator_mutex_;

  // List of all chunks allocated, for use in deallocation and contains().
  std::vector<std::span<uint8_t>> allocations_;

  // Unwind-info registrations for the chunks in `allocations_`, in the
  // same order.  Deregistered in the destructor before the chunks are
  // unmapped.  Guarded by `allocator_mutex_`.
  std::vector<std::unique_ptr<JitUnwindRegistration>>
      chunk_unwind_registrations_;

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

// pthread_atfork() handlers for the process-global linker-reserved code region.
void codeAllocatorRegionAtForkPrepare();
void codeAllocatorRegionAtForkParent();
void codeAllocatorRegionAtForkChild();

} // namespace cinderx::jit
