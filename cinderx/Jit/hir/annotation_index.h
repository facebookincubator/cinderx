// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Common/sorted_vec_map.h"

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
  static std::unique_ptr<AnnotationIndex> fromFunction(
      BorrowedRef<PyFunctionObject> func);

  // Retrieve the annotation for the given name, or return nullptr.  Pure C++;
  // safe to call with the GIL released.
  BorrowedRef<> find(BorrowedRef<> name) const;

 private:
  // Built from the flattened (name, annotation, ...) tuple used before 3.14.
  explicit AnnotationIndex(BorrowedRef<PyTupleObject> annotations);

  // Built from the __annotations__ dict used on 3.14+.
  explicit AnnotationIndex(BorrowedRef<PyDictObject> dict);

  Ref<> owner_;
  SortedVecMap<Ref<>, Ref<>> annotations_;
};

} // namespace cinderx::jit::hir
