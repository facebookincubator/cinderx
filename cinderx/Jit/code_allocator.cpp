// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_allocator.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/jit_rt.h"

#ifdef WIN32
#include <Windows.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#ifdef __aarch64__
#include <libkern/OSCacheControl.h>
#endif
#endif

#include <cstring>

namespace cinderx::jit {

using codegen::CodeSection;
using codegen::codeSectionFromName;

namespace {

#define PROPAGATE_ERROR(EXPR)                                \
  if (asmjit::Error err = (EXPR); err != asmjit::kErrorOk) { \
    return AllocateResult{nullptr, err};                     \
  }

// 2MiB to match Linux's huge-page size.
constexpr size_t kAllocSize = 1024 * 1024 * 2;

// On macOS ARM64, MAP_JIT memory requires toggling between writable and
// executable states per-thread via pthread_jit_write_protect_np.
void jitEnableWriting() {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(0);
#endif
}

void jitEnableExecuting(
    [[maybe_unused]] void* addr,
    [[maybe_unused]] size_t size,
    [[maybe_unused]] void* cold_addr = nullptr,
    [[maybe_unused]] size_t cold_size = 0) {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(addr, size);
  if (cold_size > 0) {
    sys_icache_invalidate(cold_addr, cold_size);
  }
#endif
}

bool setHugePages([[maybe_unused]] void* ptr, [[maybe_unused]] size_t size) {
#ifdef MADV_HUGEPAGE
  if (madvise(ptr, size, MADV_HUGEPAGE) == 0) {
    return true;
  }

  auto end = static_cast<void*>(static_cast<uint8_t*>(ptr) + size);
  JIT_LOG(
      "Failed to madvise [{}, {}) with MADV_HUGEPAGE, errno={}",
      ptr,
      end,
      errno);
#endif

  return false;
}

#if defined(__linux__)
// The linker script (instagram/server/native_python/linker_script.ld) reserves
// a region of address space immediately after .text for JIT code, delimited by
// these symbols. They are declared weak so that binaries built without the
// linker script (where the region doesn't exist) still link, with both symbols
// resolving to nullptr.
extern "C" {
extern char __cinder_jit_start[] __attribute__((weak));
extern char __cinder_jit_end[] __attribute__((weak));
}

// Bump-allocator state for the linker-reserved __cinder_jit region. It's global
// process data so it's protected by cinder_jit_region_mutex_.
bool s_cinder_jit_region_checked = false;
std::atomic<uint8_t*> s_cinder_jit_cur = nullptr;
size_t s_cinder_jit_free = 0;
std::mutex cinder_jit_region_mutex_;

// Prepare the linker-reserved __cinder_jit region for use. Called once on the
// first allocation. On success s_cinder_jit_cur points at the region with
// s_cinder_jit_free bytes remaining; on failure s_cinder_jit_cur stays nullptr
// and callers fall back to hinted allocation.
//
// The region comes from a `.cinder_jit (NOLOAD)` section in the linker script.
// Because that section is allocatable (SHF_ALLOC), the linker places it in a
// PT_LOAD segment, so the dynamic loader has *already mapped* this address
// range (as demand-zero anonymous pages) by the time we get here -- typically
// read-only, since the section carries no write/execute flags. We need to
// we upgrade the existing mapping's protection to RWX with mprotect.
void initCinderJitRegion() {
  // Read the weak symbols into locals so the nullptr checks are on a pointer
  // value (a weak undefined symbol resolves to nullptr) rather than the address
  // of an array, which compilers would otherwise fold to always-true.
  char* start = __cinder_jit_start;
  char* end = __cinder_jit_end;
  if (start == nullptr || end == nullptr || end <= start) {
    // Binary wasn't linked with the linker script that reserves the region.
    return;
  }

  size_t region_size = static_cast<size_t>(end - start);

  // The loader maps the region's PT_LOAD segment; make it writable and
  // executable so we can emit and run JIT code from it.
  if (mprotect(start, region_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    JIT_LOG(
        "Failed to mprotect cinder_jit region [{}, {}) as RWX, errno={}; "
        "falling back to hinted allocation",
        static_cast<void*>(start),
        static_cast<void*>(end),
        errno);
    return;
  }

  setHugePages(start, region_size);

  s_cinder_jit_cur.store(
      reinterpret_cast<uint8_t*>(start), std::memory_order_relaxed);
  s_cinder_jit_free = region_size;
}

// Bump-allocate `size` bytes from the linker-reserved __cinder_jit region.
// Returns nullptr if the region is unavailable or exhausted, in which case the
// caller falls back to hinted allocation. The returned memory is already mapped
// PROT_READ | PROT_WRITE | PROT_EXEC.
uint8_t* allocFromCinderJitRegion(size_t size) {
  if (s_cinder_jit_cur.load(std::memory_order_relaxed) == nullptr) {
    return nullptr;
  }

  std::lock_guard lock{cinder_jit_region_mutex_};
  if (size > s_cinder_jit_free) {
    return nullptr;
  }
  uint8_t* res = s_cinder_jit_cur;
  s_cinder_jit_cur.fetch_add(size, std::memory_order_relaxed);
  s_cinder_jit_free -= size;
  return res;
}
#endif // __linux__

// Allocate memory for JIT'd code.
uint8_t* allocPages(size_t size) {
#if defined(__linux__)
  if (getConfig().hinted_code_allocation) {
    // Prefer the linker-reserved region near .text when it is
    // present. This region was reserved by the linker and it's up to the
    // build system to reserve this near hot code.
    if (uint8_t* region = allocFromCinderJitRegion(size); region != nullptr) {
      return region;
    }
  }
#endif

#ifndef WIN32
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
  flags |= MAP_JIT;
#endif
  void* res =
      mmap(nullptr, size, PROT_EXEC | PROT_READ | PROT_WRITE, flags, -1, 0);
  JIT_CHECK(
      res != MAP_FAILED,
      "Failed to allocate {} bytes of memory for code",
      size);
#else
  void* res = VirtualAlloc(
      nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  JIT_CHECK(
      res != nullptr, "Failed to allocate {} bytes of memory for code", size);
#endif
  return static_cast<uint8_t*>(res);
}

} // namespace

CodeAllocator::CodeAllocator() {
#if defined(__linux__)
  std::lock_guard lock{cinder_jit_region_mutex_};
  if (!s_cinder_jit_region_checked) {
    s_cinder_jit_region_checked = true;
    initCinderJitRegion();
  }
#endif
}

ICodeAllocator* CodeAllocator::make() {
  if (getConfig().use_huge_pages) {
    return new CodeAllocatorCinder{};
  }
  return new CodeAllocator{};
}

AllocateResult CodeAllocator::addCode(asmjit::CodeHolder* code) {
  void* addr = nullptr;
  asmjit::Error error = runtime_.add(&addr, code);

  if (addr != nullptr && error == asmjit::kErrorOk) {
    used_bytes_.fetch_add(code->codeSize(), std::memory_order_relaxed);
  }

  return AllocateResult{addr, error};
}

asmjit::Error CodeAllocator::releaseCode(void* code) {
  // Find the size of the allocated region.
  asmjit::JitAllocator* inner = runtime_.allocator();
  asmjit::JitAllocator::Span span;
  if (auto error = inner->query(span, code); error != asmjit::kErrorOk) {
    return error;
  }

  if constexpr (kPyDebug) {
    auto rw = span.rw();
    if (rw != nullptr) {
      // The allocator may not actually free the code, in debug builds zero it
      // so we know the memory is freed.
      memset(rw, 0, span.size());
    }
  }

  if (auto error = runtime_.release(code); error != asmjit::kErrorOk) {
    return error;
  }

  used_bytes_.fetch_sub(span.size(), std::memory_order_relaxed);
  return asmjit::kErrorOk;
}

bool CodeAllocator::contains(const void* ptr) const {
  asmjit::JitAllocator::Span unused;
  // asmjit docs don't say that query() is thread-safe, but peeking at the
  // implementation shows that it is.
  return runtime_.allocator()->query(unused, const_cast<void*>(ptr)) ==
      asmjit::kErrorOk;
}

size_t CodeAllocator::usedBytes() const {
  return used_bytes_.load(std::memory_order_relaxed);
}

const asmjit::Environment& CodeAllocator::asmJitEnvironment() const {
  return runtime_.environment();
}

CodeAllocatorCinder::~CodeAllocatorCinder() {
  for (std::span<uint8_t> alloc : allocations_) {
#ifndef WIN32
    JIT_CHECK(
        munmap(alloc.data(), alloc.size()) == 0, "Freeing code memory failed");
#else
    VirtualFree(alloc.data(), 0, MEM_RELEASE);
#endif
  }
}

void CodeAllocatorCinder::ensureSpace(
    uint8_t*& alloc,
    size_t& alloc_free,
    size_t size,
    bool use_huge_pages) {
  if (alloc_free >= size) {
    return;
  }

  lost_bytes_.fetch_add(alloc_free, std::memory_order_relaxed);

  size_t chunk_size = ((size / kAllocSize) + 1) * kAllocSize;
  uint8_t* res = allocPages(chunk_size);
  if (use_huge_pages && setHugePages(res, chunk_size)) {
    huge_allocs_.fetch_add(1, std::memory_order_relaxed);
  } else {
    fragmented_allocs_.fetch_add(1, std::memory_order_relaxed);
  }
  alloc = res;
  allocations_.emplace_back(res, chunk_size);
  alloc_free = chunk_size;
}

void CodeAllocatorCinder::ensureSplitSpace(
    size_t hot_needed,
    size_t cold_needed) {
  if (hot_alloc_free_ >= hot_needed && cold_alloc_free_ >= cold_needed) {
    return;
  }

  // When either side needs a new allocation, allocate a single contiguous
  // region and split it. This guarantees hot and cold code are always within
  // the same mmap region, so cross-section jumps stay within ARM64's relative
  // branch range (±128MB for B/BL, ±1MB for B.cond). Without this,
  // independent mmap() calls could place hot and cold regions too far apart,
  // causing asmjit's relocateToBase()/resolveUnresolvedLinks() to fail with
  // kErrorInvalidDisplacement.
  lost_bytes_.fetch_add(
      hot_alloc_free_ + cold_alloc_free_, std::memory_order_relaxed);

  size_t total_needed = hot_needed + cold_needed;
  size_t chunk_size = ((total_needed / kAllocSize) + 1) * kAllocSize;
  uint8_t* res = allocPages(chunk_size);
  if (setHugePages(res, chunk_size)) {
    huge_allocs_.fetch_add(1, std::memory_order_relaxed);
  } else {
    fragmented_allocs_.fetch_add(1, std::memory_order_relaxed);
  }
  allocations_.emplace_back(res, chunk_size);

  // Hot code grows forward from the start, cold code grows forward from a
  // split point. Split proportionally so each side gets at least what it
  // requested, distributing any surplus evenly.
  size_t surplus = chunk_size - total_needed;
  size_t hot_share = hot_needed + surplus / 2;
  hot_alloc_ = res;
  hot_alloc_free_ = hot_share;
  cold_alloc_ = res + hot_share;
  cold_alloc_free_ = chunk_size - hot_share;
}

AllocateResult CodeAllocatorCinder::addSplitCode(asmjit::CodeHolder* code) {
  size_t hot_size = 0;
  size_t cold_size = 0;

  for (;;) {
    // Compute how much space each section type needs.
    hot_size = 0;
    cold_size = 0;
    for (asmjit::Section* section : code->sections()) {
      CodeSection cs = codeSectionFromName(section->name());
      if (cs == CodeSection::kCold) {
        cold_size += section->realSize();
      } else {
        hot_size += section->realSize();
      }
    }

    // Ensure we have enough space for both hot and cold code.
#if defined(__aarch64__)
    // On ARM64, branch displacements are limited (±128MB for B/BL, ±1MB for
    // B.cond). Allocate hot and cold from a single contiguous region so
    // cross-section jumps are always in range.
    ensureSplitSpace(hot_size, cold_size);
#else
    // On x86-64, RIP-relative addressing has a ±2GB range which is large enough
    // that independent allocations are unlikely to exceed it in practice.
    ensureSpace(hot_alloc_, hot_alloc_free_, hot_size, true);
    ensureSpace(
        cold_alloc_,
        cold_alloc_free_,
        cold_size,
        getConfig().cold_code_huge_pages);
#endif

    // Fix up offsets for each code section before resolving links.
    // All offsets are relative to the hot allocation base so that asmjit can
    // resolve cross-section jumps correctly.
    size_t hot_offset = 0;
    size_t cold_offset = static_cast<size_t>(cold_alloc_ - hot_alloc_);
    for (asmjit::Section* section : code->sections()) {
      CodeSection cs = codeSectionFromName(section->name());
      if (cs == CodeSection::kCold) {
        section->setOffset(cold_offset);
        cold_offset += section->realSize();
      } else {
        section->setOffset(hot_offset);
        hot_offset += section->realSize();
      }
    }

    bool changed = false;
    PROPAGATE_ERROR(code->ensureBranchStubIslands(&changed));
    if (!changed) {
      break;
    }
  }

  PROPAGATE_ERROR(code->resolveUnresolvedLinks());
  PROPAGATE_ERROR(code->relocateToBase(uintptr_t(hot_alloc_)));

  void* addr = hot_alloc_;
  void* cold_addr = cold_alloc_;

  // Copy each section's data to the appropriate allocation.
  size_t total_size = 0;
  jitEnableWriting();
  for (asmjit::Section* section : code->_sections) {
    size_t buffer_size = section->bufferSize();
    if (buffer_size == 0) {
      continue;
    }
    CodeSection cs = codeSectionFromName(section->name());
    if (cs == CodeSection::kCold) {
      std::memcpy(cold_alloc_, section->data(), buffer_size);
      cold_alloc_ += buffer_size;
      cold_alloc_free_ -= buffer_size;
    } else {
      std::memcpy(hot_alloc_, section->data(), buffer_size);
      hot_alloc_ += buffer_size;
      hot_alloc_free_ -= buffer_size;
    }
    total_size += buffer_size;
  }
  jitEnableExecuting(addr, hot_size, cold_addr, cold_size);

  used_bytes_.fetch_add(total_size, std::memory_order_relaxed);
  return AllocateResult{addr, asmjit::kErrorOk};
}

AllocateResult CodeAllocatorCinder::addCode(asmjit::CodeHolder* code) {
  std::lock_guard lock{allocator_mutex_};

  if (getConfig().multiple_code_sections) {
    return addSplitCode(code);
  }

  PROPAGATE_ERROR(code->flatten());
  PROPAGATE_ERROR(code->resolveUnresolvedLinks());

  size_t max_code_size = code->codeSize();
  ensureSpace(hot_alloc_, hot_alloc_free_, max_code_size, true);

  PROPAGATE_ERROR(code->relocateToBase(uintptr_t(hot_alloc_)));

  size_t actual_code_size = code->codeSize();
  JIT_CHECK(actual_code_size <= max_code_size, "Code grew during relocation");

  jitEnableWriting();
  for (asmjit::Section* section : code->_sections) {
    size_t offset = section->offset();
    size_t buffer_size = section->bufferSize();
    size_t virtual_size = section->virtualSize();

    JIT_CHECK(
        offset + buffer_size <= actual_code_size, "Inconsistent code size");
    std::memcpy(hot_alloc_ + offset, section->data(), buffer_size);

    if (virtual_size > buffer_size) {
      JIT_CHECK(
          offset + virtual_size <= actual_code_size, "Inconsistent code size");
      std::memset(
          hot_alloc_ + offset + buffer_size, 0, virtual_size - buffer_size);
    }
  }

  void* addr = hot_alloc_;
  jitEnableExecuting(addr, actual_code_size);

  hot_alloc_ += actual_code_size;
  hot_alloc_free_ -= actual_code_size;
  used_bytes_.fetch_add(actual_code_size, std::memory_order_relaxed);

  return AllocateResult{addr, asmjit::kErrorOk};
}

asmjit::Error CodeAllocatorCinder::releaseCode([[maybe_unused]] void* code) {
  // TODO(T233607793): Actually implement deallocating memory.
  return asmjit::kErrorOk;
}

bool CodeAllocatorCinder::contains(const void* ptr) const {
  std::lock_guard lock{allocator_mutex_};
  for (std::span<uint8_t> alloc : allocations_) {
    if (alloc.data() <= ptr && ptr < alloc.data() + alloc.size()) {
      return true;
    }
  }
  return false;
}

} // namespace cinderx::jit
