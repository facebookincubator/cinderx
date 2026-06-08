// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/log.h"

#include "cinderx/Jit/threaded_compile.h"

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

[[noreturn]] void abortImpl() {
  fmt::print(stderr, "\n");
  std::fflush(stderr);
  jit::printPythonException();
  std::abort();
}

} // namespace

void logImplV(
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

void abortImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args) {
  fmt::print(stderr, "JIT: {}:{} -- Abort\n", trimSourcePath(file), line);
  fmt::vprint(stderr, format, args);
  abortImpl();
}

void checkFailedImplV(
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

} // namespace jit
