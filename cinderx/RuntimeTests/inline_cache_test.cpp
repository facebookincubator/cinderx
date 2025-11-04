// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX > 0x030E0000
#include <pycore_unicodeobject.h>
#endif

#include <cstring>

class InlineCacheTest : public RuntimeTest {};

TEST_F(InlineCacheTest, LoadTypeMethodCacheLookUp) {
  const char* src = R"(
from abc import ABCMeta, abstractmethod

class RequestContext:

  @classmethod
  def class_meth(cls):
    pass

  @staticmethod
  def static_meth():
    pass

  def regular_meth():
    pass

class_meth = RequestContext.class_meth.__func__
static_meth = RequestContext.static_meth
regular_meth = RequestContext.regular_meth
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* klass = PyDict_GetItemString(locals, "RequestContext");
  ASSERT_NE(klass, nullptr) << "Couldn't get class RequestContext";

  auto py_class_meth = Ref<>::steal(PyUnicode_FromString("class_meth"));
  jit::LoadTypeMethodCache cache;
  auto res = cache.lookup(klass, py_class_meth);
  ASSERT_EQ(res.self_or_null, klass)
      << "Expected instance to be equal to class from cache look up";
  PyObject* class_meth = PyDict_GetItemString(locals, "class_meth");
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, class_meth, Py_EQ), 1)
      << "Expected method " << class_meth << " to be equal from cache lookup";
  ASSERT_EQ(cache.value(), res.callable)
      << "Expected method " << py_class_meth << " to be cached";

  for (auto& meth : {"static_meth", "regular_meth"}) {
    auto name = Ref<>::steal(PyUnicode_FromString(meth));
    jit::LoadTypeMethodCache methCache;
    auto methRes = methCache.lookup(klass, name);
    PyObject* py_meth = PyDict_GetItemString(locals, meth);
#if PY_VERSION_HEX < 0x030E0000
    ASSERT_EQ(methRes.callable, Py_None)
        << "Expected first part of cache result to be Py_None";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.self_or_null, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.self_or_null)
        << "Expected method " << meth << " to be cached";
#else
    ASSERT_EQ(methRes.self_or_null, nullptr)
        << "Expected first part of cache result to be nullptr";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.callable, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.callable)
        << "Expected method " << meth << " to be cached";
#endif
  }
}

TEST_F(InlineCacheTest, LoadModuleMethodCacheLookUp) {
  const char* src = R"(
import functools
module_meth = functools._unwrap_partial
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* functools_mod = PyDict_GetItemString(locals, "functools");
  ASSERT_NE(functools_mod, nullptr) << "Couldn't get module functools";

  PyObject* module_meth = PyDict_GetItemString(locals, "module_meth");
  ASSERT_NE(module_meth, nullptr) << "Couldn't get PyObject module_meth";

  PyObject* name_obj = PyUnicode_FromString("_unwrap_partial");
  ASSERT_NE(name_obj, nullptr) << "Couldn't create name object";
#if PY_VERSION_HEX >= 0x030E0000
  _PyUnicode_InternImmortal(PyInterpreterState_Get(), &name_obj);
#endif
  auto name = Ref<>::steal(name_obj);

  jit::LoadModuleMethodCache cache;
  auto res = cache.lookup(functools_mod, name);
#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(res.self_or_null, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(Py_None, res.callable)
      << "Expected Py_None to be returned from cache lookup";
#else
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(nullptr, res.self_or_null)
      << "Expected nullptr to be returned in self_or_null from cache lookup";
#endif

#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(cache.value(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#else
  ASSERT_EQ(PyObject_RichCompareBool(*cache.cache(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#endif
  ASSERT_EQ(
      PyObject_RichCompareBool(cache.moduleObj(), functools_mod, Py_EQ), 1)
      << "Expected functools to be cached as an obj";
}
