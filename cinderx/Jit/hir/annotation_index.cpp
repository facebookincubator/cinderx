// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/annotation_index.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"

#include <algorithm>

namespace cinderx::jit::hir {

std::unique_ptr<AnnotationIndex> AnnotationIndex::fromFunction(
    BorrowedRef<PyFunctionObject> func) {
  if (getMutableConfig().emit_type_annotation_guards) {
#if PY_VERSION_HEX >= 0x030E0000
    BorrowedRef<> annotations = PyFunction_GetAnnotations(func);
    if (!PyDict_Check(annotations)) {
      return nullptr;
    }
    BorrowedRef<PyDictObject> dict_annotations{annotations};
    return std::unique_ptr<AnnotationIndex>(
        new AnnotationIndex(dict_annotations));
#else
    if (func->func_annotations == nullptr ||
        !PyTuple_Check(func->func_annotations)) {
      return nullptr;
    }
    BorrowedRef<PyTupleObject> tuple_annotations{func->func_annotations};
    return std::unique_ptr<AnnotationIndex>(
        new AnnotationIndex(tuple_annotations));
#endif
  }
  return nullptr;
}

BorrowedRef<> AnnotationIndex::find(BorrowedRef<> name) const {
  JIT_DCHECK(
      reinterpret_cast<PyASCIIObject*>(name.getObj())->state.interned != 0,
      "should be interned");
  // annotations_ is sorted by key pointer (via std::less<Ref<>>), so this is a
  // binary search by pointer identity.  It deliberately does not call
  // SortedVecMap::find, which would require constructing an owning Ref<> and
  // thus touch the Python C-API.
  auto it = std::lower_bound(
      annotations_.begin(),
      annotations_.end(),
      name,
      [](const auto& entry, PyObject* rhs) { return entry.first.get() < rhs; });
  if (it != annotations_.end() && it->first.get() == name) {
    return it->second.get();
  }
  return nullptr;
}

// Built from the flattened (name, annotation, ...) tuple used before 3.14.
AnnotationIndex::AnnotationIndex(BorrowedRef<PyTupleObject> annotations)
    : owner_(Ref<>::create(annotations.getObj())) {
  Py_ssize_t size = PyTuple_GET_SIZE(annotations.get());
  for (Py_ssize_t index = 0; index + 1 < size; index += 2) {
    BorrowedRef<> key = PyTuple_GET_ITEM(annotations.get(), index);
    BorrowedRef<> value = PyTuple_GET_ITEM(annotations.get(), index + 1);
    annotations_.emplace(Ref<>::create(key), Ref<>::create(value));
  }
}

// Built from the __annotations__ dict used on 3.14+.
AnnotationIndex::AnnotationIndex(BorrowedRef<PyDictObject> dict)
    : owner_(Ref<>::create(dict.getObj())) {
  PyObject* key = nullptr;
  PyObject* value = nullptr;
  Py_ssize_t pos = 0;
  while (PyDict_Next(owner_, &pos, &key, &value)) {
    annotations_.emplace(Ref<>::create(key), Ref<>::create(value));
  }
}

} // namespace cinderx::jit::hir
