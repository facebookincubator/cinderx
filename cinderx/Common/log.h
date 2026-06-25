// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/config.h"

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/printf.h>

#include <cstdio>
#include <iterator>
#include <string_view>

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(gnu::cold)
#define CINDERX_COLD [[gnu::cold]]
#endif
#endif

#if !defined(CINDERX_COLD) && defined(__has_attribute)
#if __has_attribute(cold)
#define CINDERX_COLD __attribute__((cold))
#endif
#endif

#ifndef CINDERX_COLD
#define CINDERX_COLD
#endif

namespace cinderx {

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

// Set a Python RuntimeError from a C++ exception.
//
// Will replace an existing Python exception if one exists, but will log it
// first.
void setRuntimeError(const std::exception& exn);

// Outlined logging implementations to reduce code size on hot paths.
CINDERX_COLD void logImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args);
[[noreturn]] CINDERX_COLD void abortImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args);
[[noreturn]] CINDERX_COLD void checkFailedImplV(
    std::string_view file,
    int line,
    std::string_view cond_str,
    fmt::string_view format,
    fmt::format_args args);
[[noreturn]] CINDERX_COLD void throwImplV(
    std::string_view file,
    int line,
    fmt::string_view format,
    fmt::format_args args);

template <typename... Args>
CINDERX_COLD void logImpl(
    std::string_view file,
    int line,
    fmt::format_string<Args...> format,
    Args&&... args) {
  logImplV(file, line, format, fmt::make_format_args(args...));
}

template <typename... Args>
[[noreturn]] CINDERX_COLD void abortImpl(
    std::string_view file,
    int line,
    fmt::format_string<Args...> format,
    Args&&... args) {
  abortImplV(file, line, format, fmt::make_format_args(args...));
}

template <typename... Args>
[[noreturn]] CINDERX_COLD void checkFailedImpl(
    std::string_view file,
    int line,
    std::string_view cond_str,
    fmt::format_string<Args...> format,
    Args&&... args) {
  checkFailedImplV(
      file, line, cond_str, format, fmt::make_format_args(args...));
}

template <typename... Args>
[[noreturn]] CINDERX_COLD void throwImpl(
    std::string_view file,
    int line,
    fmt::format_string<Args...> format,
    Args&&... args) {
  throwImplV(file, line, format, fmt::make_format_args(args...));
}

#define JIT_LOG(...) cinderx::logImpl(__FILE__, __LINE__, __VA_ARGS__)

#define JIT_LOGIF(PRED, ...) \
  if (PRED) {                \
    JIT_LOG(__VA_ARGS__);    \
  }

#define JIT_DLOG(...) \
  JIT_LOGIF(cinderx::jit::getConfig().log.debug, __VA_ARGS__)

#define JIT_CHECK(COND, ...)                                            \
  {                                                                     \
    if (!(COND)) {                                                      \
      cinderx::checkFailedImpl(__FILE__, __LINE__, #COND, __VA_ARGS__); \
    }                                                                   \
  }

#define JIT_CHECK_ONCE(COND, ...)   \
  {                                 \
    static bool checked = false;    \
    if (!checked) {                 \
      checked = true;               \
      JIT_CHECK(COND, __VA_ARGS__); \
    }                               \
  }

#define JIT_ABORT(...) cinderx::abortImpl(__FILE__, __LINE__, __VA_ARGS__)

#define JIT_THROW(...) cinderx::throwImpl(__FILE__, __LINE__, __VA_ARGS__)

#define JIT_THROW_IF(COND, ...) \
  if (COND) {                   \
    JIT_THROW(__VA_ARGS__);     \
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

} // namespace cinderx
