// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#include <memory>
#include <string>
#include <vector>

struct HIRTestCase {
  std::string name;
  std::string src;
  std::string expected;
  bool src_is_hir{false};
  bool is_skip{false};
};

struct HIRTestSuite {
  std::string name;
  std::vector<std::string> pass_names;
  std::vector<HIRTestCase> test_cases;
};

// Read an HIR test suite specified via a text file.
//
// The text file specifies the test suite name, an optional
// optimization pass to run on the HIR, and a list of test
// cases. Each test case consists of a name, a python function
// that must be named `test`, and the expected textual HIR.
//
// File format:
//
// --- Test Suite Name ---
// <Test suite name>
// --- Passes ---
// <Optimization pass name 1>
// <Optimization pass name 2>
// ...
// --- Test Name ---
// <Test case name>
// --- Input ---
// <Python code>
// --- Expected ---
// <HIR>
// --- End ---
//
std::unique_ptr<HIRTestSuite> ReadHIRTestSuite(const std::string& path);

// flag string will be added to environment variables and a key will be
// returned, the key can be later used to remove the item via unsetenv
std::string parseAndSetEnvVar(std::string_view env_name);

// flag string will be added to XArgs dictionary and a key will be
// returned, the key can later be used to remove the item from
// the x args dictionary.
// handles 'arg=<const>' (with <const> set as the value) and just 'arg' flags
PyObject* addToXargsDict(const wchar_t* flag);
