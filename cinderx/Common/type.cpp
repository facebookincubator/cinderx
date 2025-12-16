// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/type.h"

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_typeobject.h" // @donotremove
#endif

#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

namespace jit {

std::string typeFullname(PyTypeObject* type) {
  PyObject* dict = _PyType_GetDict(type);
  PyObject* module_str =
      dict ? PyDict_GetItemString(dict, "__module__") : nullptr;
  if (module_str != nullptr && PyUnicode_Check(module_str)) {
    return fmt::format("{}:{}", unicodeAsString(module_str), type->tp_name);
  }
  return type->tp_name;
}

#if PY_VERSION_HEX >= 0x030C0000
PyObject* getBorrowedTypeDictSafe(PyTypeObject* self) {
  if (getThreadedCompileContext().compileRunning() &&
      self->tp_flags & _Py_TPFLAGS_STATIC_BUILTIN) {
    PyInterpreterState* interp = getThreadedCompileContext().interpreter();
    managed_static_type_state* state = Cix_PyStaticType_GetState(interp, self);
    return state->tp_dict;
  }
  return getBorrowedTypeDict(self);
}
#else
PyObject* getBorrowedTypeDictSafe(PyTypeObject* self) {
  return getBorrowedTypeDict(self);
}
#endif

BorrowedRef<> typeLookupSafe(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name) {
  JIT_CHECK(PyUnicode_CheckExact(name), "name must be a str");
  // Silence false positive from TSAN when checking Py_TPFLAGS_READY.
  // This flag should never change during compliation although other
  // flags may.
  ThreadedCompileSerialize guard;

  BorrowedRef<PyTupleObject> mro{type->tp_mro};
  for (size_t i = 0, n = PyTuple_GET_SIZE(mro); i < n; ++i) {
    BorrowedRef<PyTypeObject> base_ty{PyTuple_GET_ITEM(mro, i)};
    PyObject* dict = getBorrowedTypeDictSafe(base_ty);
    if (!PyType_HasFeature(base_ty, Py_TPFLAGS_READY) ||
        !hasOnlyUnicodeKeys(dict)) {
      // Abort the whole search if any base class dict is poorly-behaved
      // (before we find the name); it could contain the key we're looking for.
      return nullptr;
    }
    if (BorrowedRef<> value{PyDict_GetItemWithError(dict, name)}) {
      return value;
    }
    if constexpr (PY_VERSION_HEX < 0x030C0000) {
      JIT_CHECK(
          !PyErr_Occurred(), "Thread-unsafe exception during type lookup");
    }
  }
  return nullptr;
}

bool ensureVersionTag(BorrowedRef<PyTypeObject> type) {
  JIT_CHECK(
      getThreadedCompileContext().canAccessSharedData(),
      "Accessing type object needs lock");
  if (Ci_Type_HasValidVersionTag(type)) {
    return true;
  }
  return PyUnstable_Type_AssignVersionTag(type);
}

} // namespace jit
