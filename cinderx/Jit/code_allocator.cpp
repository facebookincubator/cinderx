// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_allocator.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/threaded_compile.h"

#include <sys/mman.h>

#include <cstring>

namespace jit {

using codegen::CodeSection;
using codegen::codeSectionFromName;

namespace {

#define PROPAGATE_ERROR(EXPR)                                \
  if (asmjit::Error err = (EXPR); err != asmjit::kErrorOk) { \
    return AllocateResult{nullptr, err};                     \
  }

// 2MiB to match Linux's huge-page size.
constexpr size_t kAllocSize = 1024 * 1024 * 2;

// Allocate memory for JIT'd code.
uint8_t* allocPages(size_t size) {
  void* res = mmap(
      nullptr,
      size,
      PROT_EXEC | PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS,
      -1,
      0);
  JIT_CHECK(
      res != MAP_FAILED,
      "Failed to allocate {} bytes of memory for code",
      size);
  return static_cast<uint8_t*>(res);
}

bool setHugePages([[maybe_unused]] void* ptr, [[maybe_unused]] size_t size) {
#ifdef MADV_HUGEPAGE
  if (madvise(ptr, size, MADV_HUGEPAGE) == 0) {
    return true;
  }

  auto end = static_cast<void*>(static_cast<uint8_t*>(ptr) + size);
  JIT_LOG(
      "Failed to madvise [{}, {}) with MADV_HUGEPAGE, errno=", ptr, end, errno);
#endif

  return false;
}

} // namespace

ICodeAllocator* CodeAllocator::make() {
  if (getConfig().multiple_code_sections) {
    return new MultipleSectionCodeAllocator{};
  } else if (getConfig().use_huge_pages) {
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
    JIT_CHECK(
        munmap(alloc.data(), alloc.size()) == 0, "Freeing code memory failed");
  }
}

AllocateResult CodeAllocatorCinder::addCode(asmjit::CodeHolder* code) {
  ThreadedCompileSerialize guard;

  PROPAGATE_ERROR(code->flatten());
  PROPAGATE_ERROR(code->resolveUnresolvedLinks());

  size_t max_code_size = code->codeSize();
  size_t alloc_size = ((max_code_size / kAllocSize) + 1) * kAllocSize;
  if (current_alloc_free_ < max_code_size) {
    lost_bytes_ += current_alloc_free_;

    uint8_t* res = allocPages(alloc_size);
    if (!setHugePages(res, alloc_size)) {
      fragmented_allocs_++;
    } else {
      huge_allocs_++;
    }
    current_alloc_ = static_cast<uint8_t*>(res);
    allocations_.emplace_back(res, alloc_size);
    current_alloc_free_ = alloc_size;
  }

  PROPAGATE_ERROR(code->relocateToBase(uintptr_t(current_alloc_)));

  size_t actual_code_size = code->codeSize();
  JIT_CHECK(actual_code_size <= max_code_size, "Code grew during relocation");

  for (asmjit::Section* section : code->_sections) {
    size_t offset = section->offset();
    size_t buffer_size = section->bufferSize();
    size_t virtual_size = section->virtualSize();

    JIT_CHECK(
        offset + buffer_size <= actual_code_size, "Inconsistent code size");
    std::memcpy(current_alloc_ + offset, section->data(), buffer_size);

    if (virtual_size > buffer_size) {
      JIT_CHECK(
          offset + virtual_size <= actual_code_size, "Inconsistent code size");
      std::memset(
          current_alloc_ + offset + buffer_size, 0, virtual_size - buffer_size);
    }
  }

  void* addr = current_alloc_;

  current_alloc_ += actual_code_size;
  current_alloc_free_ -= actual_code_size;
  used_bytes_ += actual_code_size;

  return AllocateResult{addr, asmjit::kErrorOk};
}

bool CodeAllocatorCinder::contains(const void* ptr) const {
  ThreadedCompileSerialize guard;
  for (std::span<uint8_t> alloc : allocations_) {
    if (alloc.data() <= ptr && ptr < alloc.data() + alloc.size()) {
      return true;
    }
  }
  return false;
}

MultipleSectionCodeAllocator::~MultipleSectionCodeAllocator() {
  if (code_alloc_ == nullptr) {
    return;
  }
  JIT_CHECK(
      munmap(code_alloc_, total_allocation_size_) == 0,
      "Freeing code sections failed");
}

/*
 * At startup, we allocate a contiguous chunk of memory for all code sections
 * equal to the sum of individual section sizes and subdivide internally. The
 * code is contiguously allocated internally, but logically has pointers into
 * each CodeSection.
 */
void MultipleSectionCodeAllocator::createSlabs() noexcept {
  size_t hot_section_size =
      asmjit::Support::alignUp(getConfig().hot_code_section_size, kAllocSize);
  JIT_CHECK(
      hot_section_size > 0,
      "Hot code section must have non-zero size when using multiple sections.");
  code_section_free_sizes_[CodeSection::kHot] = hot_section_size;

  size_t cold_section_size = getConfig().cold_code_section_size;
  JIT_CHECK(
      cold_section_size > 0,
      "Cold code section must have non-zero size when using multiple "
      "sections.");
  code_section_free_sizes_[CodeSection::kCold] = cold_section_size;

  total_allocation_size_ = hot_section_size + cold_section_size;

  uint8_t* region = allocPages(total_allocation_size_);
  setHugePages(region, hot_section_size);

  code_alloc_ = region;
  code_sections_[CodeSection::kHot] = region;
  region += hot_section_size;
  code_sections_[CodeSection::kCold] = region;
}

AllocateResult MultipleSectionCodeAllocator::addCode(asmjit::CodeHolder* code) {
  ThreadedCompileSerialize guard;

  if (code_sections_.empty()) {
    createSlabs();
  }

  size_t potential_code_size = code->codeSize();
  used_bytes_ += potential_code_size;
  // We fall back to the default size of code allocation if the
  // code doesn't fit into either section, and we can make this check more
  // granular by comparing sizes section-by-section.
  if (code_section_free_sizes_[CodeSection::kHot] < potential_code_size ||
      code_section_free_sizes_[CodeSection::kCold] < potential_code_size) {
    JIT_LOG(
        "Not enough memory to split code across sections, falling back to "
        "normal allocation.");
    void* addr = nullptr;
    asmjit::Error err = runtime_.add(&addr, code);
    return AllocateResult{addr, err};
  }

  // Fix up the offsets for each code section before resolving links.
  // Both the `.text` and `.addrtab` sections are written to the hot section,
  // and we need to resolve offsets between them properly.
  // In order to properly keep track of multiple text sections corresponding to
  // the same physical section to allocate to, we keep a map from
  // section->offset from start of hot section.
  std::unordered_map<CodeSection, uint64_t> offsets;
  offsets[CodeSection::kHot] = 0;
  offsets[CodeSection::kCold] =
      code_sections_[CodeSection::kCold] - code_sections_[CodeSection::kHot];

  for (asmjit::Section* section : code->sections()) {
    CodeSection code_section = codeSectionFromName(section->name());
    uint64_t offset = offsets[code_section];
    uint64_t realSize = section->realSize();
    section->setOffset(offset);
    // Since all sections lie on a contiguous slab, we rely on setting the
    // offsets of sections to allow AsmJit to properly resolve links across
    // different sections (offset 0 being the start of the hot code section).
    offsets[code_section] = offset + realSize;
  }

  // Assuming that the offsets are set properly, relocating all code to be
  // relative to the start of the hot code will ensure jumps are correct.
  PROPAGATE_ERROR(code->resolveUnresolvedLinks());
  PROPAGATE_ERROR(
      code->relocateToBase(uintptr_t(code_sections_[CodeSection::kHot])));

  // We assume that the hot section of the code is non-empty. This would be
  // incorrect for a completely cold function.
  JIT_CHECK(
      code->textSection()->realSize() > 0,
      "Every function must have a non-empty hot section.");
  void* addr = code_sections_[CodeSection::kHot];

  for (asmjit::Section* section : code->_sections) {
    size_t buffer_size = section->bufferSize();
    // Might not have generated any cold code.
    if (buffer_size == 0) {
      continue;
    }
    CodeSection code_section = codeSectionFromName(section->name());
    code_section_free_sizes_[code_section] -= buffer_size;
    std::memcpy(code_sections_[code_section], section->data(), buffer_size);
    code_sections_[code_section] += buffer_size;
  }

  return AllocateResult{addr, asmjit::kErrorOk};
}

bool MultipleSectionCodeAllocator::contains(const void* ptr) const {
  // Have to check both the hot/cold slab and the asmjit allocator.  The latter
  // is already thread-safe.
  {
    ThreadedCompileSerialize guard;
    if (code_alloc_ <= ptr && ptr < code_alloc_ + total_allocation_size_) {
      return true;
    }
  }
  return CodeAllocator::contains(ptr);
}

} // namespace jit
