// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/annotation_index.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"

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

const OwnedType* AnnotationIndex::find(BorrowedRef<> name) const {
  JIT_DCHECK(
      PyUnicode_Check(name),
      "Expecting annotation name to be a string, got {}",
      Py_TYPE(name)->tp_name);
  JIT_DCHECK(
      PyUnicode_CHECK_INTERNED(name.get()) != 0,
      "Expecting annotation name '{}' to be interned, it isn't",
      PyUnicode_AsUTF8(name));
  auto it = annotations_.find(name);
  if (it != annotations_.end() && it->first.getObj() == name.get()) {
    return &it->second;
  }
  return nullptr;
}

// Built from the flattened (name, annotation, ...) tuple used before 3.14.
AnnotationIndex::AnnotationIndex(BorrowedRef<PyTupleObject> annotations)
    : owner_(Ref<>::create(annotations.getObj())),
      origin_{Ref<>::steal(PyUnicode_InternFromString("__origin__"))} {
  Py_ssize_t size = PyTuple_GET_SIZE(annotations.get());
  for (Py_ssize_t index = 0; index + 1 < size; index += 2) {
    BorrowedRef<> key = PyTuple_GET_ITEM(annotations.get(), index);
    BorrowedRef<> value = PyTuple_GET_ITEM(annotations.get(), index + 1);
    addAnnotation(key, value);
  }
}

// Built from the __annotations__ dict used on 3.14+.
AnnotationIndex::AnnotationIndex(BorrowedRef<PyDictObject> dict)
    : owner_(Ref<>::create(dict.getObj())),
      origin_{Ref<>::steal(PyUnicode_InternFromString("__origin__"))} {
  PyObject* key = nullptr;
  PyObject* value = nullptr;
  Py_ssize_t pos = 0;
  while (PyDict_Next(owner_, &pos, &key, &value)) {
    addAnnotation(BorrowedRef<>{key}, BorrowedRef<>{value});
  }
}

void AnnotationIndex::addAnnotation(BorrowedRef<> key, BorrowedRef<> value) {
  // Verify that key is an interned unicode object.  We can't forcefully intern
  // this because it's not a reference we own, we'd have to make a copy and
  // that's probably not worth it.
  if (!PyUnicode_Check(key) || PyUnicode_CHECK_INTERNED(key) == 0) {
    return;
  }

  // Reduce parameterized generics like `list[int]` -> `list` and ensure result
  // is a type.
  auto handled = handleGeneric(value);
  if (!PyType_Check(handled.get())) {
    return;
  }

  // All type annotations will turn into exact types.  Subtypes and optional
  // types are not supported yet.
  bool optional = false;
  bool exact = true;

  OwnedType annotation{
      Ref<PyTypeObject>::steal(handled.release()), optional, exact};

  annotations_.emplace(
      Ref<PyUnicodeObject>::create(key), std::move(annotation));
}

Ref<> AnnotationIndex::handleGeneric(BorrowedRef<> annotation) {
  if (!Py_IS_TYPE(annotation, &Py_GenericAliasType)) {
    return Ref<>::create(annotation);
  }

  // Note: PyObject_GetAttr can potentially execute arbitrary Python code via
  // descriptors or __getattr__.  For builtin GenericAlias types __origin__ is a
  // simple member, so this is safe in practice, but a future improvement could
  // use a lookup that bypasses descriptor invocation (e.g. direct struct access
  // for CPython's generic_alias object or a safe tp_getattro that cannot
  // trigger user code).
  auto origin = Ref<>::steal(PyObject_GetAttr(annotation, origin_));
  if (origin == nullptr) {
    PyErr_Clear();
    return Ref<>::create(annotation);
  }
  return origin;
}

} // namespace cinderx::jit::hir
