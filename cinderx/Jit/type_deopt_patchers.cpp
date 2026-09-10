// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/type_deopt_patchers.h"

#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"

namespace cinderx::jit {

template <typename Body>
bool shouldPatchForAttr(
    BorrowedRef<PyTypeObject> old_ty,
    BorrowedRef<PyTypeObject> new_ty,
    BorrowedRef<PyUnicodeObject> attr_name,
    Body body) {
  if (new_ty != old_ty) {
    // new_ty is not the same as old_ty (it's either nullptr or a new type). If
    // new_ty has the same attribute with the same properties, we could watch
    // it as well and leave the specialized code in place, but that would
    // increase complexity and memory usage for what should be a vanishingly
    // rare situation.
    return true;
  }

  // Similarly to the JIT code using this patcher, we want to avoid triggering
  // user-visible side-effects, so we do the lookup using typeLookupSafe(). If
  // that succeeds and returns an object that still satisfies our requirements,
  // we attempt to give the type a new version tag before declaring success.
  BorrowedRef<> attr{typeLookupSafe(new_ty, attr_name)};
  return body(attr) || !PyUnstable_Type_AssignVersionTag(new_ty);
}

TypeDeoptPatcher::TypeDeoptPatcher(BorrowedRef<PyTypeObject> type)
    : type_{type} {}

bool TypeDeoptPatcher::maybePatch(BorrowedRef<PyTypeObject>) {
  patch();
  return true;
}

bool TypeDeoptPatcher::assumptionsStillValid() const {
  // The generic patcher fires on any change to the type, which cannot be
  // re-checked after the fact (see the class comment in the header).
  return true;
}

BorrowedRef<PyTypeObject> TypeDeoptPatcher::type() const {
  return type_;
}

void TypeDeoptPatcher::onUnpatch() {
  JIT_ABORT(
      "TypeDeoptPatcher for type {} being unpatched but that's not supported!",
      type_->tp_name);
}

TypeAttrDeoptPatcher::TypeAttrDeoptPatcher(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<PyUnicodeObject> attr_name,
    BorrowedRef<> target_object)
    : TypeDeoptPatcher{type} {
  attr_name_ = attr_name;
  target_object_ = target_object;
}

bool TypeAttrDeoptPatcher::maybePatch(BorrowedRef<PyTypeObject> new_ty) {
  bool should_patch =
      shouldPatchForAttr(type_, new_ty, attr_name_, [&](BorrowedRef<> attr) {
        return attr != target_object_;
      });
  if (should_patch) {
    patch();
  }
  return should_patch;
}

bool TypeAttrDeoptPatcher::assumptionsStillValid() const {
  // Mirrors the check in maybePatch(): the attribute must still resolve to
  // the object the compiled code specialized on. Uses _PyType_Lookup()
  // directly: the GIL is held, so unlike during compilation there is no need
  // for typeLookupSafe()'s GIL handling, and the exact lookup avoids its
  // false negatives.
  return _PyType_Lookup(type_, attr_name_) == target_object_.get();
}

void TypeAttrDeoptPatcher::onPatch() {
  attr_name_ = nullptr;
  target_object_ = nullptr;
}

SplitDictDeoptPatcher::SplitDictDeoptPatcher(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<PyUnicodeObject> attr_name,
    PyDictKeysObject* keys)
    : TypeDeoptPatcher{type}, keys_{keys} {
  attr_name_ = attr_name;
}

bool SplitDictDeoptPatcher::maybePatch(BorrowedRef<PyTypeObject> new_ty) {
  bool should_patch =
      shouldPatchForAttr(type_, new_ty, attr_name_, [&](BorrowedRef<> attr) {
        if (attr != nullptr) {
          // This is more conservative than strictly necessary: the split dict
          // lookup would still be OK if attr isn't a data descriptor, but we'd
          // have to watch attr's type to safely rely on that fact.
          return true;
        }

        return !hasOurSharedKeys(new_ty);
      });
  if (should_patch) {
    patch();
  }
  return should_patch;
}

bool SplitDictDeoptPatcher::hasOurSharedKeys(
    BorrowedRef<PyTypeObject> type) const {
  if (!PyType_HasFeature(type_, Py_TPFLAGS_HEAPTYPE)) {
    return false;
  }
  return BorrowedRef<PyHeapTypeObject>(type_)->ht_cached_keys == keys_;
}

bool SplitDictDeoptPatcher::assumptionsStillValid() const {
  // Mirrors the check in maybePatch(): no descriptor may have appeared at the
  // attribute name and the shared keys must be unchanged. Uses _PyType_Lookup()
  // directly, as above; an exact lookup matters here because a false-negative
  // miss would wrongly validate.
  if (_PyType_Lookup(type_, attr_name_) != nullptr) {
    return false;
  }
  return hasOurSharedKeys(type_);
}

void SplitDictDeoptPatcher::onPatch() {
  attr_name_ = nullptr;
}

} // namespace cinderx::jit
