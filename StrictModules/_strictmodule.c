/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include <Python.h>

// Including this here is a hack until _strictmodule.c is migrated to CinderX
#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif
#include "cinderx/StrictModules/pystrictmodule.h"
#include "cinderx/StrictModules/strict_module_checker_interface.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#ifndef Py_LIMITED_API
#ifdef __cplusplus
extern "C" {
#endif

PyDoc_STRVAR(strictmodule_doc, "Strict Module related types and methods");

static int strictmodule_exec(PyObject* m) {
  if (PyType_Ready(&Ci_StrictModuleLoader_Type) < 0)
    goto fail;
  if (PyType_Ready(&Ci_StrictModuleAnalysisResult_Type) < 0)
    goto fail;

  Py_INCREF(&Ci_StrictModuleLoader_Type);
  if (PyModule_AddObject(
          m, "StrictModuleLoader", (PyObject*)&Ci_StrictModuleLoader_Type) <
      0) {
    Py_DECREF(&Ci_StrictModuleLoader_Type);
    return -1;
  }
  Py_INCREF(&Ci_StrictModuleAnalysisResult_Type);
  if (PyModule_AddObject(
          m,
          "StrictAnalysisResult",
          (PyObject*)&Ci_StrictModuleAnalysisResult_Type) < 0) {
    Py_DECREF(&Ci_StrictModuleAnalysisResult_Type);
    return -1;
  }
  PyObject* val;
#define SET_STR(name)                          \
  val = PyUnicode_FromString(Ci_##name);       \
  if (val == NULL) {                           \
    return -1;                                 \
  }                                            \
  if (PyModule_AddObject(m, #name, val) < 0) { \
    Py_DECREF(val);                            \
    return -1;                                 \
  }
#undef SET_STR
#define SET_LONG(name)                         \
  val = PyLong_FromLong(Ci_##name);            \
  if (val == NULL) {                           \
    return -1;                                 \
  }                                            \
  if (PyModule_AddObject(m, #name, val) < 0) { \
    Py_DECREF(val);                            \
    return -1;                                 \
  }
  SET_LONG(STRICT_MODULE_KIND)
  SET_LONG(STATIC_MODULE_KIND)
  SET_LONG(NONSTRICT_MODULE_KIND)
  SET_LONG(STUB_KIND_MASK_NONE)
  SET_LONG(STUB_KIND_MASK_ALLOWLIST)
  SET_LONG(STUB_KIND_MASK_TYPING)
  SET_LONG(STUB_KIND_MASK_STRICT)
#undef SET_LONG
  return 0;
fail:
  Py_XDECREF(m);
  return -1;
}

static struct PyModuleDef_Slot strictmodule_slots[] = {
    {Py_mod_exec, strictmodule_exec},
    {0, NULL},
};

static struct PyModuleDef strictmodulemodule = {
    PyModuleDef_HEAD_INIT,
    "_strictmodule",
    strictmodule_doc,
    0,
    NULL,
    strictmodule_slots,
    NULL,
    NULL,
    NULL};

/* Export function for the module (*must* be called PyInit_strictmodule) */

PyMODINIT_FUNC PyInit__strictmodule(void) {
  return PyModuleDef_Init(&strictmodulemodule);
}

#ifdef __cplusplus
}
#endif
#endif /* Py_LIMITED_API */
