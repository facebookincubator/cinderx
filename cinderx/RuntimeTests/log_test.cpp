// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/log.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <stdexcept>

using SetRuntimeErrorTest = RuntimeTest;

using namespace cinderx;

namespace {

// Consume the currently-raised exception and return str() of it, clearing the
// error indicator so it doesn't leak into the next test or teardown.
std::string takeRaisedExceptionMessage() {
  Ref<> raised = Ref<>::steal(PyErr_GetRaisedException());
  if (raised == nullptr) {
    return "<no exception raised>";
  }
  Ref<> str = Ref<>::steal(PyObject_Str(raised));
  if (str == nullptr) {
    return "<failed to stringify exception>";
  }
  return PyUnicode_AsUTF8(str);
}

} // namespace

TEST_F(SetRuntimeErrorTest, RaisesRuntimeErrorWithExceptionMessage) {
  ASSERT_EQ(PyErr_Occurred(), nullptr);

  setRuntimeError(std::runtime_error{"compilation went sideways"});

  ASSERT_NE(PyErr_Occurred(), nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  EXPECT_EQ(takeRaisedExceptionMessage(), "compilation went sideways");
}

TEST_F(SetRuntimeErrorTest, ReplacesExistingPythonException) {
  // A Python exception is already pending when the C++ exception arrives.
  PyErr_SetString(PyExc_ValueError, "the original Python error");
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));

  setRuntimeError(std::runtime_error{"the C++ error wins"});

  // The original ValueError is gone, replaced by a RuntimeError carrying the
  // C++ exception's message.
  ASSERT_NE(PyErr_Occurred(), nullptr);
  EXPECT_FALSE(PyErr_ExceptionMatches(PyExc_ValueError));
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  EXPECT_EQ(takeRaisedExceptionMessage(), "the C++ error wins");
}
