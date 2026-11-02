// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/py_object_allocator.h"

#include "cinderx/python.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/log.h"

#include <cstring>

namespace cinderx {

namespace {

std::pair<PyObject*, size_t> allocateFromHeap(
    BorrowedRef<PyTypeObject> tp,
    Py_ssize_t nitems) {
  const size_t size = _PyObject_VAR_SIZE(tp.get(), nitems);
  PyObject* obj = PyObject_GC_NewVar(PyObject, tp.get(), nitems);
  // See comment in CPython's gc_alloc() about failure.
  JIT_CHECK(obj != nullptr, "Failed to allocate PyObject");
  return {obj, size};
}

} // namespace

PyObjectAllocator::PyObjectAllocator(PyObjectAllocatorConfig config)
    : config_{config} {
  if (config_.entry_count == 0 || config_.entry_size == 0) {
    return;
  }
  JIT_THROW_IF(
      config_.entry_size < sizeof(Entry),
      "PyObjectAllocator entry_size {} is too small",
      config_.entry_size);
  JIT_THROW_IF(
      config_.entry_size % alignof(Entry) != 0,
      "PyObjectAllocator entry_size {} is not aligned to {}",
      config_.entry_size,
      alignof(Entry));
  arena_ =
      std::make_unique<std::byte[]>(config_.entry_count * config_.entry_size);
  Entry* next = nullptr;
  for (size_t i = 0; i < config_.entry_count; ++i) {
    Entry* entry =
        reinterpret_cast<Entry*>(arena_.get() + i * config_.entry_size);
    entry->next = next;
    next = entry;
  }
  head_ = next;
}

void PyObjectAllocator::setKeepAlive(BorrowedRef<> obj) {
  keep_alive_ = obj;
}

std::pair<PyObject*, size_t> PyObjectAllocator::allocateVar(
    BorrowedRef<PyTypeObject> tp,
    Py_ssize_t nitems) {
  JIT_DCHECK(
      PyType_HasFeature(tp.get(), Py_TPFLAGS_HAVE_GC),
      "PyObjectAllocator only allocates GC-tracked objects");

  const size_t size = _PyObject_VAR_SIZE(tp.get(), nitems);
  const size_t total_size = sizeof(PyGC_Head) + size;

  if (head_ == nullptr || total_size > config_.entry_size) {
    return allocateFromHeap(tp, nitems);
  }

  // The pooled path hands out raw storage with the GC header layout below,
  // and free() relies on it.  Heap allocations (including every allocation
  // when the storage is disabled, e.g. in free-threaded builds where the
  // pre-header layout differs) do not have this constraint.
  // We *assume* these assertions hold in free().
  JIT_DCHECK_ONCE(
      _PyType_PreHeaderSize(tp.get()) == sizeof(PyGC_Head) &&
          !_PyType_HasFeature(tp.get(), Py_TPFLAGS_PREHEADER),
      "Unexpected pre-header setup");

  void* raw = rawAllocate();
  // Zero the pre-header, which in this case is the GC header.  The
  // reference for this is gc_alloc() + _PyObject_GC_Link().  It would be
  // nice if the latter were public so we could custom allocate GC'able
  // objects.
  // Note we are NOT bumping the GC's young generation counter here as
  // _PyObject_GC_Link would.  I argue we're not actually increasing
  // memory pressure so this is not needed.
  std::memset(raw, 0, sizeof(PyGC_Head));
  PyVarObject* op = reinterpret_cast<PyVarObject*>(
      reinterpret_cast<uintptr_t>(raw) + sizeof(PyGC_Head));

  _PyObject_InitVar(op, tp.get(), nitems);

  return {reinterpret_cast<PyObject*>(op), size};
}

void PyObjectAllocator::free(PyObject* ptr) {
  if (!fromThisArena(ptr)) {
    PyObject_GC_Del(ptr);
    return;
  }
  Entry* entry = reinterpret_cast<Entry*>(
      reinterpret_cast<uintptr_t>(ptr) - sizeof(PyGC_Head));
  JIT_DCHECK(
      (reinterpret_cast<uintptr_t>(entry) -
       reinterpret_cast<uintptr_t>(arena_.get())) %
              config_.entry_size ==
          0,
      "Incorrect pointer calculation");
  entry->next = head_;
  head_ = entry;
  Py_XDECREF(keep_alive_);
}

void* PyObjectAllocator::rawAllocate() {
  JIT_DCHECK(head_ != nullptr, "No free PyObjectAllocator entries");
  Entry* entry = head_;
  head_ = entry->next;
  Py_XINCREF(keep_alive_);
  return entry;
}

bool PyObjectAllocator::fromThisArena(const void* ptr) const {
  if (arena_ == nullptr) {
    return false;
  }
  const std::byte* begin = arena_.get();
  const std::byte* end = begin + config_.entry_count * config_.entry_size;
  const auto* byte_ptr = static_cast<const std::byte*>(ptr);
  return byte_ptr >= begin && byte_ptr < end;
}

} // namespace cinderx
