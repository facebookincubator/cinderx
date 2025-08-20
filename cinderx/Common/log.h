// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/threaded_compile.h"

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/printf.h>

#include <cstdio>
#include <iterator>

namespace jit {

template <typename... Args>
auto format_to(
    std::string& s,
    fmt::format_string<Args...> format,
    Args&&... args) {
  return fmt::format_to(
      std::back_inserter(s), format, std::forward<Args>(args)...);
}

// Print the current Python exception to stderr, if it exists.
void printPythonException();

// Use PyObject_Repr() to get a string representation of a PyObject. Use with
// caution - this can end up executing arbitrary Python code. Always succeeds
// but may return a description of an error in string e.g.
// "<failed to get UTF8 from Python string>"
std::string repr(BorrowedRef<> obj);

#define JIT_LOG(...)                                           \
  {                                                            \
    FILE* _output = jit::getConfig().log.output_file;          \
    jit::ThreadedCompileSerialize guard;                       \
    fmt::print(_output, "JIT: {}:{} -- ", __FILE__, __LINE__); \
    fmt::print(_output, __VA_ARGS__);                          \
    fmt::print(_output, "\n");                                 \
    std::fflush(_output);                                      \
  }

#define JIT_LOGIF(PRED, ...) \
  if (PRED) {                \
    JIT_LOG(__VA_ARGS__);    \
  }

#define JIT_DLOG(...) JIT_LOGIF(jit::getConfig().log.debug, __VA_ARGS__)

#define JIT_CHECK(COND, ...)                      \
  {                                               \
    if (!(COND)) {                                \
      fmt::print(                                 \
          stderr,                                 \
          "JIT: {}:{} -- Assertion failed: {}\n", \
          __FILE__,                               \
          __LINE__,                               \
          #COND);                                 \
      JIT_ABORT_IMPL(__VA_ARGS__);                \
    }                                             \
  }

#define JIT_CHECK_ONCE(COND, ...)   \
  {                                 \
    static bool checked = false;    \
    if (!checked) {                 \
      JIT_CHECK(COND, __VA_ARGS__); \
    } else {                        \
      checked = true;               \
    }                               \
  }

#define JIT_ABORT(...)                                               \
  {                                                                  \
    fmt::print(stderr, "JIT: {}:{} -- Abort\n", __FILE__, __LINE__); \
    JIT_ABORT_IMPL(__VA_ARGS__);                                     \
  }

#define JIT_ABORT_IMPL(...)          \
  {                                  \
    fmt::print(stderr, __VA_ARGS__); \
    fmt::print(stderr, "\n");        \
    std::fflush(stderr);             \
    jit::printPythonException();     \
    std::abort();                    \
  }

#ifdef Py_DEBUG
#define JIT_DABORT(...) JIT_ABORT(__VA_ARGS__)
#define JIT_DCHECK(COND, ...) JIT_CHECK((COND), __VA_ARGS__)
#define JIT_DCHECK_ONCE(COND, ...) JIT_CHECK_ONCE((COND), __VA_ARGS__)
#else
#define JIT_DABORT(...)     \
  if (0) {                  \
    JIT_ABORT(__VA_ARGS__); \
  }
#define JIT_DCHECK(COND, ...)       \
  if (0) {                          \
    JIT_CHECK((COND), __VA_ARGS__); \
  }
#define JIT_DCHECK_ONCE(COND, ...)       \
  if (0) {                               \
    JIT_CHECK_ONCE((COND), __VA_ARGS__); \
  }
#endif

} // namespace jit
