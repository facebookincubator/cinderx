/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/Common/import.h"

#include "cinderx/Common/ref.h"

PyObject* _Ci_CreateBuiltinModule(PyModuleDef* def, const char* name) {
  cinderx::Ref<> machinery =
      cinderx::Ref<>::steal(PyImport_ImportModule("importlib.machinery"));
  if (machinery == nullptr) {
    return nullptr;
  }
  cinderx::Ref<> spec_type =
      cinderx::Ref<>::steal(PyObject_GetAttrString(machinery, "ModuleSpec"));
  if (spec_type == nullptr) {
    return nullptr;
  }
  cinderx::Ref<> module_name =
      cinderx::Ref<>::steal(PyUnicode_FromString(name));
  if (module_name == nullptr) {
    return nullptr;
  }

  PyObject* args[] = {module_name, Py_None};
  cinderx::Ref<> module_spec =
      cinderx::Ref<>::steal(PyObject_Vectorcall(spec_type, args, 2, nullptr));
  if (module_spec == nullptr) {
    return nullptr;
  }

  cinderx::Ref<> mod =
      cinderx::Ref<>::steal(PyModule_FromDefAndSpec(def, module_spec));
  if (mod == nullptr) {
    return nullptr;
  }

  if (PyModule_ExecDef(mod, def) < 0) {
    return nullptr;
  }

  cinderx::BorrowedRef<> modules = PyImport_GetModuleDict();
  if (PyDict_SetItem(modules, module_name, mod) < 0) {
    return nullptr;
  }
  return mod.release();
}
