/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/Common/import.h"

#include "cinderx/Common/ref.h"

PyObject* _Ci_CreateBuiltinModule(PyModuleDef* def, const char* name) {
  Ref<> machinery = Ref<>::steal(PyImport_ImportModule("importlib.machinery"));
  if (machinery == nullptr) {
    return nullptr;
  }
  Ref<> spec_type =
      Ref<>::steal(PyObject_GetAttrString(machinery, "ModuleSpec"));
  if (spec_type == nullptr) {
    return nullptr;
  }
  Ref<> module_name = Ref<>::steal(PyUnicode_FromString(name));
  if (module_name == nullptr) {
    return nullptr;
  }

  PyObject* args[] = {module_name, Py_None};
  Ref<> module_spec =
      Ref<>::steal(PyObject_Vectorcall(spec_type, args, 2, nullptr));
  if (module_spec == nullptr) {
    return nullptr;
  }

  Ref<> mod = Ref<>::steal(PyModule_FromDefAndSpec(def, module_spec));
  if (mod == nullptr) {
    return nullptr;
  }

  if (PyModule_ExecDef(mod, def) < 0) {
    return nullptr;
  }

  BorrowedRef<> modules = PyImport_GetModuleDict();
  if (PyDict_SetItem(modules, module_name, mod) < 0) {
    return nullptr;
  }
  return mod.release();
}
