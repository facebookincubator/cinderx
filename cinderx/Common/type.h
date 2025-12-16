// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

#include <string>

namespace jit {

// When possible, return the fully qualified name of the given type (including
// its module). Falls back to the type's bare name.
std::string typeFullname(PyTypeObject* type);

// Simulate _PyType_Lookup(), but in a way that should avoid any heap mutations
// (caches, refcount operations, arbitrary code execution).
//
// Since this function is very conservative in the operations it will perform,
// it may return false negatives; a nullptr return does *not* mean that
// _PyType_Lookup() will also return nullptr. However, a non-nullptr return
// value should be the same value _PyType_Lookup() would return.
BorrowedRef<> typeLookupSafe(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name);

// Attempt to ensure that the given type has a valid version tag, returning
// true if successful.
bool ensureVersionTag(BorrowedRef<PyTypeObject> type);

} // namespace jit
