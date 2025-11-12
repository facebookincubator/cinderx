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

constexpr std::string_view kDelimPrefix = "---";
constexpr std::string_view kDisabledPrefix = "@disabled";

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

struct Reader {
  explicit Reader(std::ifstream& file, const std::string& path)
      : file{file}, path{path} {}

  char peekChar() {
    return file.peek();
  }

  std::string readLine() {
    std::string s;
    if (!std::getline(file, s)) {
      err("Failed reading file {} at line {}, {}",
          path,
          line_num + 1,
          strerror(errno));
    }
    line_num++;
    return s;
  }

  // Parse out the message inside of a delimiter line.
  std::string_view parseDelim(std::string_view line) {
    if (!line.starts_with(kDelimPrefix)) {
      err("Expected delimiter in file {} at line {} does not start with {}",
          path,
          line_num,
          kDelimPrefix);
    }
    if (!line.ends_with(kDelimPrefix)) {
      err("Expected delimiter in file {} at line {} does not end with {}",
          path,
          line_num,
          kDelimPrefix);
    }
    if (line.size() <= kDelimPrefix.size() * 2) {
      err("Expected delimiter in file {} at line {} is too short: {}",
          path,
          line_num,
          line);
    }
    line.remove_prefix(kDelimPrefix.size());
    line.remove_suffix(kDelimPrefix.size());
    trimWhitespace(line);

    return line;
  }

  // Match a line to a delimiter with a given message inside of it.
  void matchDelim(std::string_view line, std::string_view expected) {
    if (parseDelim(line) != expected) {
      err("Expected delimiter in file {} at line {} to contain '{}', but it is "
          "'{}'",
          path,
          line_num,
          expected,
          line);
    }
  }

  void readDelim(std::string_view expected) {
    matchDelim(readLine(), expected);
  }

  std::string readUntilDelim() {
    std::ostringstream os;
    while (!isExhausted() && peekChar() != kDelimPrefix[0]) {
      os << readLine() << '\n';
    }
    return os.str();
  }

  bool isExhausted() const {
    return file.eof();
  }

  template <class... Args>
  [[noreturn]] void err(fmt::format_string<Args...> fmt, Args&&... args) {
    throw std::runtime_error{fmt::format(fmt, std::forward<Args>(args)...)};
  }

  std::ifstream& file;
  std::string path;
  int line_num{0};
};

// Initialize a test suite with a name and a list of passes to run.
std::unique_ptr<HIRTestSuite> initTestSuite(Reader& reader) {
  auto suite = std::make_unique<HIRTestSuite>();

  reader.readDelim("Test Suite Name");
  suite->name = reader.readLine();

  reader.readDelim("Passes");

  while (!reader.isExhausted() && reader.peekChar() != kDelimPrefix[0]) {
    suite->pass_names.push_back(reader.readLine());
  }

  return suite;
}

enum class ScanStatus {
  End,
  Skip,
  NextTest,
  Ok,
};

// Scan through sections of "--- Expected $PY_VERSION ---" until the correct one
// is found, or until the next section.
ScanStatus scanUntilExpected(Reader& reader, std::string_view delim) {
  const std::string expected_prefix = fmt::format("{} Expected", kDelimPrefix);

  std::string line;
  while (true) {
    line = reader.readLine();
    if (!line.starts_with(kDelimPrefix)) {
      continue;
    }
    if (line == delim) {
      return ScanStatus::Ok;
    }
    if (reader.parseDelim(line) == "End") {
      return ScanStatus::End;
    }
    if (line == "--- Test Name ---") {
      return ScanStatus::NextTest;
    }
    if (line == "--- Skip ---") {
      return ScanStatus::Skip;
    }
    if (line.starts_with(expected_prefix)) {
      continue;
    }
    reader.err(
        "Unrecognized '{}' in file {} at line {} when looking for expected HIR "
        "output",
        line,
        reader.path,
        reader.line_num);
  }

  // Unreachable, reader will find what it needs or it will error on EOF.
  std::abort();
}

} // namespace

std::unique_ptr<HIRTestSuite> ReadHIRTestSuite(const std::string& suite_path) {
  std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path().append(
          suite_path);

  std::ifstream file;
  file.open(path);
  if (file.fail()) {
    throw std::runtime_error{
        fmt::format("Failed opening test data file: {}", strerror(errno))};
  }
  FileGuard g(file);
  Reader reader(file, path);

  auto suite = initTestSuite(reader);

  const std::string expected_prefix = fmt::format("{} Expected", kDelimPrefix);
  const std::string expected_delimiter = fmt::format(
      "{} {}.{} {}",
      expected_prefix,
      PY_MAJOR_VERSION,
      PY_MINOR_VERSION,
      kDelimPrefix);

  reader.readDelim("Test Name");

  while (true) {
    std::string name_buf = reader.readLine();
    std::string_view name = name_buf;
    trimWhitespace(name);

    reader.readDelim("Input");

    std::string src_buf = reader.readUntilDelim();
    std::string_view src = src_buf;

    // Parse out the HIR tag if it exists.
    bool src_is_hir = false;
    constexpr std::string_view kHIRTag = "# HIR\n";
    if (src.starts_with(kHIRTag)) {
      src_is_hir = true;
      src.remove_prefix(kHIRTag.size());
    }

    bool is_skip = false;
    bool missing = false;
    bool is_end = false;

    // Scan until the correct expected block.
    switch (scanUntilExpected(reader, expected_delimiter)) {
      case ScanStatus::Skip:
        is_skip = true;
        break;
      case ScanStatus::End:
        is_end = true;
        [[fallthrough]];
      case ScanStatus::NextTest:
        missing = true;
        break;
      case ScanStatus::Ok:
        break;
    }

    std::string hir = missing || is_skip ? "" : reader.readUntilDelim();

    if (!is_skip) {
      HIRTestCase& test_case = suite->test_cases.emplace_back();
      test_case.name = std::string{name};
      test_case.src = src;
      test_case.expected = std::string{hir};
      test_case.src_is_hir = src_is_hir;
      // A disabled test will be flagged by the test system as
      // skipped (rather than excluded from view which is what our "-- Skip --"
      // does).
      test_case.is_skip = test_case.name.starts_with(kDisabledPrefix);
    }

    if (is_end) {
      return suite;
    }

    if (missing) {
      continue;
    }

    // Now have to scan past any other expected outputs afterwards until we hit
    // the next test or the end.
    while (true) {
      switch (scanUntilExpected(reader, expected_delimiter)) {
        case ScanStatus::End:
          return suite;
        case ScanStatus::Skip:
          // Need to keep scanning to find the next test/eof.
          continue;
        case ScanStatus::NextTest:
          break;
        case ScanStatus::Ok:
          reader.err(
              "Test '{}.{}' has two '{}' sections",
              suite->name,
              name,
              expected_delimiter);
      }
      break;
    }
  }

  return suite;
}

std::string parseAndSetEnvVar(std::string_view env_name) {
  auto delim_pos = env_name.find('=');
  std::string key{env_name.substr(0, delim_pos)};
  std::string value{
      delim_pos == env_name.npos ? "1" : env_name.substr(delim_pos + 1)};
  setenv(key.c_str(), value.c_str(), 1);
  return key;
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
