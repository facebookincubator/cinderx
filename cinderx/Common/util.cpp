// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/config.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cinderx::jit {

static_assert(kPyObjectPtrTag == 0);

#ifdef Py_GIL_DISABLED
static_assert((kDeferredRcTag & kPyObjectTagBits) == kDeferredRcTag);
static_assert(std::has_single_bit(kDeferredRcTag));
#endif

} // namespace cinderx::jit

namespace cinderx {

const void* getStablePointer(const void* ptr) {
  return jit::getConfig().use_stable_pointers
      ? reinterpret_cast<const void*>(0xdeadbeef)
      : ptr;
}

void setUseStablePointers(bool enable) {
  jit::getMutableConfig().use_stable_pointers = enable;
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

} // namespace cinderx
