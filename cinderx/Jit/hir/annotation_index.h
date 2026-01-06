// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

#include <memory>

namespace jit::hir {

// When building type annotation guards, we have to find the annotations by
// specific names. For short lists, we can iterate directly through the tuple.
// However, once it gets big enough, it becomes more efficient to build a
// dictionary and loop through that instead.
class AnnotationIndex {
 public:
  // Retrieve the annotation for the given name, or return nullptr.
  PyObject* find(PyObject* name) {
    if (dict_) {
      return PyDict_GetItem(dict_, name);
    }
    for (Py_ssize_t index = 0; index < size_; index += 2) {
      if (name == PyTuple_GET_ITEM(annotations_, index)) {
        return PyTuple_GET_ITEM(annotations_, index + 1);
      }
    }
    return nullptr;
  }

  static std::unique_ptr<AnnotationIndex> from_function(
      BorrowedRef<PyFunctionObject> func);

 private:
  explicit AnnotationIndex(BorrowedRef<PyTupleObject> annotations)
      : annotations_(annotations) {
    size_ = PyTuple_GET_SIZE(annotations_);
    if (size_ >= 16) {
      dict_ = Ref<>::steal(PyDict_New());
      for (Py_ssize_t index = 0; index < size_; index += 2) {
        PyObject* key = PyTuple_GET_ITEM(annotations_, index);
        PyObject* value = PyTuple_GET_ITEM(annotations_, index + 1);
        PyDict_SetItem(dict_, key, value);
      }
    }
  }

  explicit AnnotationIndex(BorrowedRef<PyDictObject> dict)
      : dict_(Ref<>::create((PyObject*)dict)) {}

  BorrowedRef<PyTupleObject> annotations_;
  Ref<> dict_ = nullptr;
  Py_ssize_t size_;
};

} // namespace jit::hir
