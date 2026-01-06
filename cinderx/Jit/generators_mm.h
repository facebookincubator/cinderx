// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/generators_mm_iface.h"

#include <array>

namespace jit {

struct JitGenObject;

// These values were determined experimentally on IG's webservers by utilizing
// the stats above. The number of outstanding requests seems to burst up to ~60k
// on startup but then quickly settles down to around 1-2k, so 2048 entries
// should be enough. The average size seems to be ~400 bytes with the max being
// about 10x that. Performance experiments showed a size of 512 was a greater
// improvement compared to 1024. Presumably the trade off in extra fixed memory
// allocation cost on workers isn't worth it for greater sizes.
constexpr size_t kGenFreeListEntries = 2048;
constexpr size_t kGenFreeListEntrySize = 512;

// Basically a free-list but the backing memory is pre-allocated in a single
// block. This makes it possible to determine if the storage is from this pool
// even after deopt by just examining a generator's pointer value.
class JitGenFreeList : public IJitGenFreeList {
 public:
  JitGenFreeList();
  ~JitGenFreeList() override = default;

  std::pair<JitGenObject*, size_t> allocate(
      BorrowedRef<PyCodeObject> code,
      uint64_t jit_spill_words) override;
  void free(PyObject* ptr) override;

 private:
  void* rawAllocate();
  bool fromThisArena(void* ptr);

  struct Entry {
    union {
      uint8_t data[kGenFreeListEntrySize];
      Entry* next;
    };
  };

  std::array<Entry, kGenFreeListEntries> entries_;
  Entry* head_;
};

} // namespace jit

#endif // PY_VERSION_HEX >= 0x030C0000
