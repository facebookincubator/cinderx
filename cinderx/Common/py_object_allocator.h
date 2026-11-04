// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

#include <cstddef>
#include <memory>
#include <utility>

namespace cinderx {

// Configuration for `PyObjectAllocator`.
struct PyObjectAllocatorConfig {
  // Number of objects the allocator keeps storage for at once.  Zero means to
  // disable internal storage entirely and to serve all allocations from the
  // CPython heap.
  size_t entry_count{0};
  // Size in bytes of a single storage slot.  An allocation whose total size
  // (including the GC header) exceeds this falls back to the CPython heap.
  size_t entry_size{0};
};

// Allocates Python objects of arbitrary type and size.
//
// Has its own internal logic for managing allocations efficiently, but falls
// back to the allocating from the CPython heap when needed.  This means the
// caller must also hold an attached thread state, in case a GC collection is
// triggered.
//
// This allocator is only thread-safe when it falls back to the CPython heap.
class PyObjectAllocator {
 public:
  explicit PyObjectAllocator(PyObjectAllocatorConfig config);
  ~PyObjectAllocator() = default;

  PyObjectAllocator(const PyObjectAllocator&) = delete;
  PyObjectAllocator& operator=(const PyObjectAllocator&) = delete;
  PyObjectAllocator(PyObjectAllocator&&) = delete;
  PyObjectAllocator& operator=(PyObjectAllocator&&) = delete;

  // Set an object to be reference-counted once per object allocated from this
  // allocator, from allocation until free() or releaseKeepAlive().
  //
  // Used to ensure the CinderX module outlives any objects allocated from this
  // allocator.  Ideally we would not need this.
  void setKeepAlive(BorrowedRef<> obj);

  // Drop the keep-alive reference held for `ptr` before it is freed, if it was
  // allocated from the CPython heap; internal storage holds its reference until
  // free().  May release the last reference to the keep-alive object, which can
  // destroy this allocator.
  void releaseKeepAlive(PyObject* ptr);

  // Allocate a Python object with a variable number of item slots (like
  // `PyObject_GC_NewVar`).  Return the object and its size in bytes, as
  // computed by `_PyObject_VAR_SIZE(tp, nitems)`.
  std::pair<PyObject*, size_t> allocateVar(
      BorrowedRef<PyTypeObject> tp,
      Py_ssize_t nitems);

  // Release an object allocated by this allocator.
  void free(PyObject* ptr);

 private:
  struct Entry {
    Entry* next;
  };

  void* rawAllocate();
  bool fromThisArena(const void* ptr) const;

  PyObjectAllocatorConfig config_;
  std::unique_ptr<std::byte[]> arena_;
  Entry* head_{nullptr};
  PyObject* keep_alive_{nullptr};
};

} // namespace cinderx
