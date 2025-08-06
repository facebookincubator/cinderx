// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "internal/pycore_object.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/generators_mm.h"
#include "cinderx/module_state.h"
#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interpframe.h"
#endif

namespace jit {

JitGenFreeList:: // NOLINT(cppcoreguidelines-pro-type-member-init)
    JitGenFreeList() {
  Entry* next = nullptr;
  for (size_t i = 0; i < kGenFreeListEntries; ++i) {
    entries_[i].next = next;
    next = &entries_[i];
  }
  head_ = next;
}

void* JitGenFreeList::rawAllocate() {
  JIT_DCHECK(head_, "No free generator entries");
  Entry* entry = head_;
  head_ = entry->next;
  // The memory for the free-list is backed by the module state, so bump the
  // reference count to prevent it being free'd before all free-listed
  // generators are.
  Py_INCREF(cinderx::getModuleState()->module());
  return entry->data;
}

bool JitGenFreeList::fromThisArena(void* ptr) {
  return ptr >= &entries_ && ptr < &entries_[kGenFreeListEntries - 1] + 1;
}

void JitGenFreeList::free(PyObject* ptr) {
  if (!fromThisArena(ptr)) {
    PyObject_GC_Del(ptr);
    return;
  }
  // Note we assert in allocate() that the "presize" of data is
  // sizeof(PyGC_HEAD)
  Entry* entry = reinterpret_cast<Entry*>( // NOLINT(performance-no-int-to-ptr)
      reinterpret_cast<uintptr_t>(ptr) - sizeof(PyGC_Head));
  JIT_DCHECK(
      (reinterpret_cast<uintptr_t>(entry) -
       reinterpret_cast<uintptr_t>(&entries_[0])) %
              kGenFreeListEntrySize ==
          0,
      "Incorrect pointer calculation");
  entry->next = head_;
  head_ = entry;
  // See comment in rawAllocate()
  Py_DECREF(cinderx::getModuleState()->module());
}

std::pair<JitGenObject*, size_t> JitGenFreeList::allocate(
    BorrowedRef<PyCodeObject> code,
    uint64_t jit_data_size) {
  BorrowedRef<PyTypeObject> gen_tp = cinderx::getModuleState()->genType();
  // We *assume* these assertions hold in free().
  JIT_DCHECK_ONCE(
      _PyType_PreHeaderSize(gen_tp) == sizeof(PyGC_Head) &&
          !_PyType_HasFeature(gen_tp, Py_TPFLAGS_PREHEADER),
      "Unexpected pre-header setup");

  // A "slot" is the size of PyObject* and we assume this just means 64 bits for
  // purposes of sizing allocation to cover JIT data.
  static_assert(sizeof(uint64_t) == sizeof(PyObject*));

  // +1 for the pointer to JIT data (GenDataFooter*)
  size_t slots =
      _PyFrame_NumSlotsForCodeObject(code) + 1 + ceilDiv(jit_data_size, 8);
  // All the generator types should be the same size.
  size_t size = _PyObject_VAR_SIZE(gen_tp, slots);
  size_t total_size = sizeof(PyGC_Head) + size;

  bool is_coro = !!(code->co_flags & CO_COROUTINE);

  if (!head_ || total_size > kGenFreeListEntrySize) {
    JitGenObject* gen = is_coro
        ? reinterpret_cast<JitGenObject*>(PyObject_GC_NewVar(
              PyCoroObject, cinderx::getModuleState()->coroType(), slots))
        : reinterpret_cast<JitGenObject*>(
              PyObject_GC_NewVar(PyGenObject, gen_tp, slots));
    // See comment in allocate_and_link_interpreter_frame about failure.
    JIT_CHECK(gen != nullptr, "Failed to allocate JitGenObject");
    return {gen, size};
  }

  void* raw = rawAllocate();
  // Zero the pre-header, which in this case is the GC header. The
  // The reference for this is gc_alloc() + _PyObject_GC_Link(). It
  // would be nice if the latter were public so we could custom
  // allocate GC'able objects.
  // Note we are NOT bumping the GC's young generation counter here as
  // _PyObject_GC_Link would. I argue we're not actually increasing memory
  // pressure so this is not needed.
  (reinterpret_cast<PyObject**>(raw))[0] = nullptr;
  (reinterpret_cast<PyObject**>(raw))[1] = nullptr;
  PyVarObject* op =
      reinterpret_cast<PyVarObject*>( // NOLINT(performance-no-int-to-ptr)
          reinterpret_cast<uintptr_t>(raw) + sizeof(PyGC_Head));

  PyTypeObject* tp = is_coro ? cinderx::getModuleState()->coroType() : gen_tp;
  _PyObject_InitVar(op, tp, slots);

  return {reinterpret_cast<JitGenObject*>(op), size};
}

} // namespace jit

#endif // PY_VERSION_HEX >= 0x030C0000
