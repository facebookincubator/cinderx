// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/annotation_index.h"

#include "cinderx/Jit/config.h"

namespace jit::hir {

std::unique_ptr<AnnotationIndex> AnnotationIndex::from_function(
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

} // namespace jit::hir
