// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/sorted_vec_map.h"

#include <algorithm>
#include <memory>

namespace cinderx::jit::hir {

// Maps argument names to their type annotations so the HIR builder can emit
// argument type guards.
//
// The annotation snapshot is built once, in the constructor, while the GIL is
// held (during preload).  find() then reads only C++ state, so it is safe to
// call from a background compile that builds HIR with the GIL released -- it
// must never touch the Python C-API.
//
// Lookups compare names by pointer identity.  Argument names (from
// co_varnames) and annotation keys are interned, so identity matches what
// value-equality would find; a name that is somehow not interned simply
// produces no guard, which is safe (guards are an optimization, not required
// for correctness).
class AnnotationIndex {
 public:
  // Retrieve the annotation for the given name, or return nullptr.  Pure C++;
  // safe to call with the GIL released.
  //
  // annotations_ is sorted by key pointer (via std::less<Ref<>>), so this is a
  // binary search by pointer identity.  It deliberately does not call
  // SortedVecMap::find, which would require constructing an owning Ref<> and
  // thus touch the Python C-API.
  BorrowedRef<> find(PyObject* name) const {
    JIT_DCHECK(
        reinterpret_cast<PyASCIIObject*>(name)->state.interned != 0,
        "should be interned");
    auto it = std::lower_bound(
        annotations_.begin(),
        annotations_.end(),
        name,
        [](const auto& entry, PyObject* rhs) {
          return entry.first.get() < rhs;
        });
    if (it != annotations_.end() && it->first.get() == name) {
      return it->second.get();
    }
    return nullptr;
  }

  static std::unique_ptr<AnnotationIndex> fromFunction(
      BorrowedRef<PyFunctionObject> func);

 private:
  // Built from the flattened (name, annotation, ...) tuple used before 3.14.
  explicit AnnotationIndex(BorrowedRef<PyTupleObject> annotations)
      : owner_(Ref<>::create(annotations.getObj())) {
    Py_ssize_t size = PyTuple_GET_SIZE(annotations.get());
    for (Py_ssize_t index = 0; index + 1 < size; index += 2) {
      BorrowedRef<> key = PyTuple_GET_ITEM(annotations.get(), index);
      BorrowedRef<> value = PyTuple_GET_ITEM(annotations.get(), index + 1);
      annotations_.emplace(Ref<>::create(key), Ref<>::create(value));
    }
  }

  // Built from the __annotations__ dict used on 3.14+.
  explicit AnnotationIndex(BorrowedRef<PyDictObject> dict)
      : owner_(Ref<>::create(dict.getObj())) {
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(owner_, &pos, &key, &value)) {
      annotations_.emplace(Ref<>::create(key), Ref<>::create(value));
    }
  }

  Ref<> owner_;
  SortedVecMap<Ref<>, Ref<>> annotations_;
};

} // namespace cinderx::jit::hir
