// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/log.h"

#include <stdexcept>

namespace cinderx {

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

[[noreturn]] CINDERX_COLD void abortImpl() {
  fmt::print(stderr, "\n");
  std::fflush(stderr);
  printPythonException();
  std::abort();
}

} // namespace

CINDERX_COLD void logImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  FILE* output = jit::getConfig().log.output_file;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock{mutex};
  fmt::print(output, "JIT: {}:{} -- ", trimSourcePath(file), line);
  fmt::vprint(output, format, args);
  fmt::print(output, "\n");
  std::fflush(output);
}

[[noreturn]] CINDERX_COLD void abortImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  fmt::print(stderr, "JIT: {}:{} -- Abort\n", trimSourcePath(file), line);
  fmt::vprint(stderr, format, args);
  abortImpl();
}

[[noreturn]] CINDERX_COLD void checkFailedImplV(
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

[[noreturn]] CINDERX_COLD void throwImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  std::string msg = fmt::format("{}:{} ", trimSourcePath(file), line);
  fmt::vformat_to(std::back_inserter(msg), format, args);
  throw std::runtime_error{msg};
}

void printPythonException() {
  // This can run on a background compile thread that does not hold the GIL
  // (e.g. a JIT_CHECK firing mid-compile). Touching the Python error indicator
  // without the GIL is unsafe, so only report when the GIL is held.
#if PY_VERSION_HEX >= 0x030E0000
  if (PyThreadState_GetUnchecked() == nullptr) {
#else
  if (_PyThreadState_UncheckedGet() == nullptr) {
#endif
    return;
  }
  if (PyErr_Occurred()) {
    auto exc = Ref<>::steal(PyErr_GetRaisedException());
    PyErr_DisplayException(exc);
  }
}

std::string repr(BorrowedRef<> obj) {
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

} // namespace cinderx
