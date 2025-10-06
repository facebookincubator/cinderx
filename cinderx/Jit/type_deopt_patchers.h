// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_patcher.h"

namespace jit {

// Patch a DeoptPatchpoint when the given PyTypeObject changes at all. This
// should only be used (instead of a more specific subclass) in cases where it
// is impossible to check the property we care about in maybePatch() (e.g., if
// the change to the type happens after PyType_Modified() is called).
class TypeDeoptPatcher : public JumpPatcher {
 public:
  explicit TypeDeoptPatcher(BorrowedRef<PyTypeObject> type);

  virtual bool maybePatch(BorrowedRef<PyTypeObject> new_ty);

  // Access the type being watched.
  BorrowedRef<PyTypeObject> type() const;

 protected:
  void onUnpatch() override;

  // The type being watched.  It outlives this object because this object will
  // be cleaned up by a type watcher notification.
  BorrowedRef<PyTypeObject> type_;
};

// Patch a DeoptPatchpoint when the given PyTypeObject no longer has the given
// PyObject* at the specified name.
class TypeAttrDeoptPatcher : public TypeDeoptPatcher {
 public:
  TypeAttrDeoptPatcher(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<PyUnicodeObject> attr_name,
      BorrowedRef<> target_object);

  bool maybePatch(BorrowedRef<PyTypeObject> new_ty) override;

 private:
  void onPatch() override;

  Ref<PyUnicodeObject> attr_name_;
  Ref<> target_object_;
};

class SplitDictDeoptPatcher : public TypeDeoptPatcher {
 public:
  SplitDictDeoptPatcher(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<PyUnicodeObject> attr_name,
      PyDictKeysObject* keys);

  bool maybePatch(BorrowedRef<PyTypeObject> new_ty) override;

 private:
  void onPatch() override;

  Ref<PyUnicodeObject> attr_name_;

  // We don't need to hold a strong reference to keys_ like we do for
  // attr_name_ because calls to PyTypeModified() happen before the old keys
  // object is decrefed.
  PyDictKeysObject* keys_;
};

} // namespace jit
