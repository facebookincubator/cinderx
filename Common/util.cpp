// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_typeobject.h" // @donotremove
#endif

#include "cinderx/Common/code.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr size_t INITIAL_SIZE = 104;

struct jit_string_t {
  char* str;
  size_t capacity;
  size_t pos;
  char _string[INITIAL_SIZE];
};

jit_string_t* ss_alloc() {
  jit_string_t* ss = (jit_string_t*)malloc(sizeof(jit_string_t));

  ss->capacity = INITIAL_SIZE;
  ss->pos = 0;
  ss->str = ss->_string;
  return ss;
}

void ss_free(jit_string_t* ss) {
  if (ss->str != ss->_string) {
    free(ss->str);
  }
  free(ss);
}

void ss_reset(jit_string_t* ss) {
  ss->pos = 0;
}

const char* ss_get_string(const jit_string_t* ss) {
  return ss->str;
}

const char* ss_get_string(const auto_jit_string_t& ss) {
  return ss_get_string(ss.get());
}

int ss_is_empty(const jit_string_t* ss) {
  return ss->pos == 0;
}

int ss_vsprintf(jit_string_t* ss, const char* format, va_list args) {
  while (1) {
    int free_space = ss->capacity - ss->pos;

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(ss->str + ss->pos, free_space, format, args_copy);
    va_end(args_copy);

    if (free_space > len) {
      ss->pos += len;
      return len;
    }

    if (ss->str != ss->_string) {
      ss->capacity *= 2;
      ss->str = (char*)realloc(ss->str, ss->capacity);
    } else {
      ss->capacity = 256;
      ss->str = (char*)malloc(ss->capacity);
      memcpy(ss->str, ss->_string, ss->pos);
    }
    JIT_CHECK(
        ss->str != nullptr,
        "Unable to allocate memory size = {} bytes",
        ss->capacity);
  }
}

int ss_sprintf(jit_string_t* ss, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int n = ss_vsprintf(ss, format, args);
  va_end(args);
  return n;
}

jit_string_t* ss_sprintf_alloc(const char* format, ...) {
  jit_string_t* ss = ss_alloc();
  va_list args;
  va_start(args, format);
  ss_vsprintf(ss, format, args);
  va_end(args);
  return ss;
}

namespace jit {

static bool s_use_stable_pointers{false};

const void* getStablePointer(const void* ptr) {
  return s_use_stable_pointers ? reinterpret_cast<const void*>(0xdeadbeef)
                               : ptr;
}

void setUseStablePointers(bool enable) {
  s_use_stable_pointers = enable;
}

static std::string fullnameImpl(PyObject* module, PyObject* qualname) {
  auto safe_str = [](BorrowedRef<> str) {
    if (str == nullptr || !PyUnicode_Check(str)) {
      return "<invalid>";
    }
    return PyUnicode_AsUTF8(str);
  };
  return fmt::format("{}:{}", safe_str(module), safe_str(qualname));
}

std::string codeFullname(PyObject* module, PyCodeObject* code) {
  return fullnameImpl(module, code->co_qualname);
}

std::string funcFullname(PyFunctionObject* func) {
  return fullnameImpl(func->func_module, func->func_qualname);
}

PyObject* getVarnameTuple(PyCodeObject* code, int* idx) {
  if (*idx < code->co_nlocals) {
    return PyCode_GetVarnames(code);
  }

  *idx -= code->co_nlocals;
  auto ncellvars = PyTuple_GET_SIZE(PyCode_GetCellvars(code));
  if (*idx < ncellvars) {
    return PyCode_GetCellvars(code);
  }

  *idx -= ncellvars;
  return PyCode_GetFreevars(code);
}

PyObject* getVarname(PyCodeObject* code, int idx) {
#if PY_VERSION_HEX >= 0x030C0000
  return PyTuple_GET_ITEM(code->co_localsplusnames, idx);
#else
  PyObject* tuple = getVarnameTuple(code, &idx);
  return PyTuple_GET_ITEM(tuple, idx);
#endif
}

std::string unicodeAsString(PyObject* str) {
  Py_ssize_t size;
  const char* utf8 = PyUnicode_AsUTF8AndSize(str, &size);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return "";
  }
  return std::string(utf8, size);
}

Ref<> stringAsUnicode(std::string_view str) {
  return Ref<>::steal(PyUnicode_FromStringAndSize(str.data(), str.size()));
}

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
  if (PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
    return true;
  }
  return PyUnstable_Type_AssignVersionTag(type);
}

uint32_t hashBytecode(BorrowedRef<PyCodeObject> code) {
  uint32_t crc = crc32(0, nullptr, 0);
  BorrowedRef<> bc = PyCode_GetCode(code);
  if (!PyBytes_Check(bc)) {
    return crc;
  }

  char* buffer;
  Py_ssize_t len;
  if (PyBytes_AsStringAndSize(bc, &buffer, &len) < 0) {
    return crc;
  }

  return crc32(crc, reinterpret_cast<unsigned char*>(buffer), len);
}

std::string codeQualname(BorrowedRef<PyCodeObject> code) {
  if (code->co_qualname != nullptr) {
    return unicodeAsString(code->co_qualname);
  }
  if (code->co_name != nullptr) {
    return unicodeAsString(code->co_name);
  }
  return "<unknown>";
}

} // namespace jit
