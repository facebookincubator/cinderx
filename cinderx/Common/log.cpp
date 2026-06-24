// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/log.h"

#include "cinderx/Jit/threaded_compile.h"

#include <stdexcept>

namespace jit {

namespace {

// Trim file paths to be rooted at "cinderx/" for cleaner log output.
std::string_view trimSourcePath(std::string_view path) {
  constexpr std::string_view pattern =
#ifdef _WIN32
      "cinderx\\"
#else
      "cinderx/"
#endif
      ;
  size_t pos = path.rfind(pattern);
  return pos != std::string_view::npos ? path.substr(pos) : path;
}

[[noreturn]] JIT_COLD void abortImpl() {
  fmt::print(stderr, "\n");
  std::fflush(stderr);
  jit::printPythonException();
  std::abort();
}

} // namespace

JIT_COLD void logImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  FILE* output = getConfig().log.output_file;
  ThreadedCompileSerialize guard;
  fmt::print(output, "JIT: {}:{} -- ", trimSourcePath(file), line);
  fmt::vprint(output, format, args);
  fmt::print(output, "\n");
  std::fflush(output);
}

[[noreturn]] JIT_COLD void abortImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  fmt::print(stderr, "JIT: {}:{} -- Abort\n", trimSourcePath(file), line);
  fmt::vprint(stderr, format, args);
  abortImpl();
}

[[noreturn]] JIT_COLD void checkFailedImplV(
    std::string_view file,
    int line,
    std::string_view cond_str,
    fmt::string_view format,
    fmt::format_args args) {
  fmt::print(
      stderr,
      "JIT: {}:{} -- Assertion failed: {}\n",
      trimSourcePath(file),
      line,
      cond_str);
  fmt::vprint(stderr, format, args);
  abortImpl();
}

[[noreturn]] JIT_COLD void throwImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  std::string msg = fmt::format("{}:{} ", trimSourcePath(file), line);
  fmt::vformat_to(std::back_inserter(msg), format, args);
  throw std::runtime_error{msg};
}

void printPythonException() {
  if (PyErr_Occurred()) {
    auto exc = Ref<>::steal(PyErr_GetRaisedException());
    PyErr_DisplayException(exc);
  }
}

std::string repr(BorrowedRef<> obj) {
  jit::ThreadedCompileSerialize guard;

  PyObject *t, *v, *tb;

  PyErr_Fetch(&t, &v, &tb);
  auto p_str =
      Ref<PyObject>::steal(PyObject_Repr(const_cast<PyObject*>(obj.get())));
  PyErr_Restore(t, v, tb);
  if (p_str == nullptr) {
    return fmt::format(
        "<failed to repr Python object of type %s>", Py_TYPE(obj)->tp_name);
  }
  Py_ssize_t len;
  const char* str = PyUnicode_AsUTF8AndSize(p_str, &len);
  if (str == nullptr) {
    return "<failed to get UTF8 from Python string>";
  }
  return {str, static_cast<std::string::size_type>(len)};
}

void setRuntimeError(const std::exception& exn) {
  // Shouldn't happen, but in case we doubled up on Python and C++ exceptions,
  // make sure to log the Python exception first, then override it with the C++
  // exception.  Otherwise it would just be lost.
  if (auto err = Ref<>::steal(PyErr_GetRaisedException())) {
    PyErr_DisplayException(err);
  }
  PyErr_SetString(PyExc_RuntimeError, exn.what());
}

} // namespace jit
