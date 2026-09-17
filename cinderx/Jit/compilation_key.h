// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"

namespace cinderx::jit {
// Lookup key for compiled functions in Context: a code object and the globals
// and builtins dicts it was JIT-compiled with.
struct CompilationKey {
  // These three are borrowed references; the values are kept alive by strong
  // references in the corresponding CodeRuntime.
  PyObject* code;
  PyObject* builtins;
  PyObject* globals;

  explicit CompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{func->func_code},
        builtins{func->func_builtins},
        globals{func->func_globals} {}

  CompilationKey(PyObject* code, PyObject* builtins, PyObject* globals)
      : code(code), builtins(builtins), globals(globals) {}

  constexpr bool operator==(const CompilationKey& other) const = default;
};

struct OwnedCompilationKey {
  Ref<> code;
  Ref<> builtins;
  Ref<> globals;

  explicit OwnedCompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{Ref<>::create(func->func_code)},
        builtins{Ref<>::create(func->func_builtins)},
        globals{Ref<>::create(func->func_globals)} {}

  OwnedCompilationKey(Ref<> code, Ref<> builtins, Ref<> globals)
      : code{std::move(code)},
        builtins{std::move(builtins)},
        globals{std::move(globals)} {}

  bool operator==(const OwnedCompilationKey& other) const = default;
};

} // namespace cinderx::jit

template <>
struct std::hash<cinderx::jit::CompilationKey> {
  std::size_t operator()(const cinderx::jit::CompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return cinderx::combineHash(
        hasher(key.code), hasher(key.globals), hasher(key.builtins));
  }
};

template <>
struct std::hash<cinderx::jit::OwnedCompilationKey> {
  std::size_t operator()(const cinderx::jit::OwnedCompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return cinderx::combineHash(
        hasher(key.code.get()),
        hasher(key.globals.get()),
        hasher(key.builtins.get()));
  }
};
