// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace cinderx {

using namespace cinderx::jit;

class CheckBannedCallsTest : public RuntimeTest {
 public:
  Result compile(const char* src) {
    Ref<PyFunctionObject> func{compileAndGet(src, "test")};
    return compileFunction(func);
  }

  Ref<> callTest() {
    return Ref<>::steal(PyObject_CallNoArgs(getGlobal("test")));
  }
};

TEST_F(CheckBannedCallsTest, ExecWithoutGlobalsNotCompiled) {
  const char* src = R"(
def test():
  exec("x = 1")
)";
  EXPECT_NE(compile(src), Result::OK);
}

TEST_F(CheckBannedCallsTest, EvalWithoutLocalsNotCompiled) {
  const char* src = R"(
def test(g):
  return eval("1", g)
)";
  EXPECT_NE(compile(src), Result::OK);
}

TEST_F(CheckBannedCallsTest, ExecWithStarArgsNotCompiled) {
  const char* src = R"(
def test(args):
  exec(*args)
)";
  EXPECT_NE(compile(src), Result::OK);
}

TEST_F(CheckBannedCallsTest, AliasedExecNotCompiled) {
  const char* src = R"(
run = exec
def test():
  run("x = 1")
)";
  EXPECT_NE(compile(src), Result::OK);
}

TEST_F(CheckBannedCallsTest, ExecWithGlobalsAndLocalsCompiled) {
  const char* src = R"(
def test():
  ns = {}
  exec("x = 1", {}, ns)
  return ns["x"]
)";
  ASSERT_EQ(compile(src), Result::OK);
  Ref<> result = callTest();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 1);
}

TEST_F(CheckBannedCallsTest, EvalWithKeywordGlobalsAndLocalsCompiled) {
  const char* src = R"(
def test():
  return eval("y", globals={}, locals={"y": 1})
)";
  EXPECT_EQ(compile(src), Result::OK);
}

TEST_F(CheckBannedCallsTest, ShadowedExecCompiled) {
  const char* src = R"(
def exec(s):
  return s
def test():
  return exec("x")
)";
  ASSERT_EQ(compile(src), Result::OK);
  Ref<> result = callTest();
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(result, "x"), 0);
}

} // namespace cinderx
