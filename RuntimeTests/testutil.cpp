// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/RuntimeTests/testutil.h"

#include <fmt/format.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kDelim = "---";

template <class T>
std::ostream& err(const T& path) {
  return std::cerr << "ERROR [" << path << "]: ";
}

bool read_delim(std::ifstream& /* file */) {
  return false;
}

// Remove whitespace from the beginning and end of a string.
void trimWhitespace(std::string_view& s) {
  while (!s.empty() && std::isspace(s[0])) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(s.back())) {
    s.remove_suffix(1);
  }
}

struct FileGuard {
  explicit FileGuard(std::ifstream& f) : file(f) {}
  ~FileGuard() {
    file.close();
  }
  std::ifstream& file;
};

struct Result {
  std::string error;

  static Result Ok() {
    return Result{};
  }

  template <class... Args>
  static Result Error(fmt::format_string<Args...> fmt, Args&&... args) {
    return Result{fmt::format(fmt, std::forward<Args>(args)...)};
  }

  static Result ErrorFromErrno(std::string_view prefix) {
    return Result{fmt::format("{}: {}", prefix, strerror(errno))};
  }

  bool isError() const {
    return !error.empty();
  }
};

struct Reader {
  explicit Reader(const std::string& p, std::ifstream& f)
      : path(p), file(f), line_num(0) {}

  char PeekChar() {
    return file.peek();
  }

  Result ReadLine(std::string& s) {
    if (!std::getline(file, s)) {
      return Result::ErrorFromErrno("Failed reading line");
    }
    line_num++;
    return Result::Ok();
  }

  // Parse out the message inside of a delimiter line.
  Result ParseDelim(std::string_view& line) {
    if (!line.starts_with(kDelim)) {
      return Result::Error(
          "Expected delimiter at line {} does not start with {}",
          line_num,
          kDelim);
    }
    if (!line.ends_with(kDelim)) {
      return Result::Error(
          "Expected delimiter at line {} does not end with {}",
          line_num,
          kDelim);
    }
    if (line.size() <= kDelim.size() * 2) {
      return Result::Error(
          "Expected delimiter at line {} is too short: {}", line_num, line);
    }
    line.remove_prefix(kDelim.size());
    line.remove_suffix(kDelim.size());
    trimWhitespace(line);
    return Result::Ok();
  }

  // Match a line to a delimiter with a given message inside of it.
  Result MatchDelim(std::string_view line, std::string_view expected) {
    ParseDelim(line);
    if (line != expected) {
      return Result::Error(
          "Expected delimiter at line {} to contain '{}', but it is '{}'",
          line_num,
          expected,
          line);
    }
    return Result::Ok();
  }

  Result ReadDelim(std::string_view expected) {
    std::string line;
    auto result = ReadLine(line);
    if (result.isError()) {
      return result;
    }
    return MatchDelim(line, expected);
  }

  Result ReadUntilDelim(std::string& s) {
    std::ostringstream os;
    Result result;
    while (!IsExhausted() && PeekChar() != kDelim[0]) {
      std::string line;
      auto result = ReadLine(line);
      if (result.isError()) {
        s = os.str();
        return result;
      }
      os << line << std::endl;
    }
    s = os.str();
    return Result::Ok();
  }

  bool IsExhausted() const {
    return file.eof();
  }

  const std::string& path;
  std::ifstream& file;
  int line_num;
};

} // namespace

std::unique_ptr<HIRTestSuite> ReadHIRTestSuite(const std::string& suite_path) {
  std::ifstream file;

  std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path().append(
          suite_path);

  file.open(path);
  if (file.fail()) {
    err(path) << "Failed opening test data file: " << strerror(errno)
              << std::endl;
    return nullptr;
  }
  FileGuard g(file);

  auto suite = std::make_unique<HIRTestSuite>();
  Reader reader(path, file);

  Result result = reader.ReadDelim("Test Suite Name");
  if (result.isError()) {
    err(path) << "Failed reading first line of the file: " << result.error
              << std::endl;
    return nullptr;
  }
  result = reader.ReadLine(suite->name);
  if (result.isError()) {
    err(path) << "Failed reading test suite name: " << result.error
              << std::endl;
    return nullptr;
  }

  result = reader.ReadDelim("Passes");
  if (result.isError()) {
    err(path) << result.error << std::endl;
    return nullptr;
  }

  while (!reader.IsExhausted() && reader.PeekChar() != kDelim[0]) {
    std::string pass_name;
    result = reader.ReadLine(pass_name);
    if (result.isError()) {
      err(path) << "Failed reading pass name: " << result.error << std::endl;
      return nullptr;
    }
    suite->pass_names.push_back(pass_name);
  }

  while (!reader.IsExhausted()) {
    std::string line;

    std::string name_buf;
    result = reader.ReadLine(line);
    if (result.isError()) {
      err(path) << "Failed to read test case or end delimiter line: "
                << result.error << std::endl;
      return nullptr;
    }

    auto name_result = reader.MatchDelim(line, "Test Name");
    auto end_result = reader.MatchDelim(line, "End");
    if (!end_result.isError()) {
      break;
    }
    if (name_result.isError()) {
      err(path) << result.error << std::endl;
      return nullptr;
    }

    result = reader.ReadLine(name_buf);
    if (result.isError()) {
      err(path) << "Failed to read out test name: " << result.error
                << std::endl;
      return nullptr;
    }

    std::string_view name = name_buf;
    trimWhitespace(name);

    std::string src;
    result = reader.ReadDelim("Input");
    if (result.isError()) {
      err(path) << result.error << std::endl;
      return nullptr;
    }
    result = reader.ReadUntilDelim(src);
    if (result.isError()) {
      err(path) << "Failed reading test case: " << result.error << std::endl;
      return nullptr;
    }

    auto src_is_hir = false;
    const char* hir_tag = "# HIR\n";
    if (src.substr(0, strlen(hir_tag)) == hir_tag) {
      src_is_hir = true;
      src = src.substr(strlen(hir_tag));
    }

    result = reader.ReadDelim("Expected");
    if (result.isError()) {
      err(path) << result.error << std::endl;
      return nullptr;
    }
    std::string hir;
    result = reader.ReadUntilDelim(hir);
    if (result.isError()) {
      err(path) << "Failed reading test case: " << result.error << std::endl;
      return nullptr;
    }

    suite->test_cases.emplace_back(std::string{name}, src_is_hir, src, hir);
  }

  return suite;
}

const char* parseAndSetEnvVar(const char* env_name) {
  if (strchr(env_name, '=')) {
    const char* key = strtok(strdup(env_name), "=");
    const char* value = strtok(nullptr, "=");
    setenv(key, value, 1);
    return key;
  } else {
    setenv(env_name, "1", 1);
    return env_name;
  }
}

PyObject* addToXargsDict(const wchar_t* flag) {
  PyObject *key = nullptr, *value = nullptr;

  PyObject* opts = PySys_GetXOptions();

  const wchar_t* key_end = wcschr(flag, L'=');
  if (!key_end) {
    key = PyUnicode_FromWideChar(flag, -1);
    value = Py_True; // PyUnicode_FromWideChar(flag, -1);
    Py_INCREF(value);
  } else {
    key = PyUnicode_FromWideChar(flag, key_end - flag);
    value = PyUnicode_FromWideChar(key_end + 1, -1);
  }

  PyDict_SetItem(opts, key, value);
  Py_DECREF(value);

  // we will need the object later on...
  return key;
}
