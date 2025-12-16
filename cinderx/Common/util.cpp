// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_typeobject.h" // @donotremove
#endif

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"

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

} // namespace jit
