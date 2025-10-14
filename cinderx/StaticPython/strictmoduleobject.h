// Copyright (c) Meta Platforms, Inc. and affiliates.
/* Module object interface */

#ifndef Ci_MODULEOBJECT_H
#define Ci_MODULEOBJECT_H

#include "cinderx/python.h"

#include "internal/pycore_moduleobject.h"

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject Ci_StrictModule_Type;

#define Ci_StrictModule_Check(op) PyObject_TypeCheck(op, &Ci_StrictModule_Type)
#define Ci_StrictModule_CheckExact(op) (Py_TYPE(op) == &Ci_StrictModule_Type)

PyObject* Ci_StrictModule_New(PyTypeObject*, PyObject*, PyObject*);

#if !defined(Py_LIMITED_API)
PyObject* Ci_StrictModule_GetOriginal(PyObject* obj, PyObject* name);
int Ci_do_strictmodule_patch(PyObject* self, PyObject* name, PyObject* value);
PyObject* Ci_StrictModule_GetDictSetter(PyObject*);
PyObject* Ci_StrictModule_GetDict(PyObject*);
#endif

typedef struct {
  PyObject_HEAD
  PyObject* globals;
  PyObject* global_setter;
  PyObject* originals;
  PyObject* static_thunks;
  PyObject* imported_from;
  PyObject* weaklist;
} Ci_StrictModuleObject;

static inline PyObject* Ci_MaybeStrictModule_Dict(PyObject* op) {
  if (Ci_StrictModule_Check(op)) {
    return ((Ci_StrictModuleObject*)op)->globals;
  }

  return ((PyModuleObject*)op)->md_dict;
}

static inline PyObject* Ci_StrictModuleGetDict(PyObject* mod) {
  assert(Ci_StrictModule_Check(mod));
  return ((Ci_StrictModuleObject*)mod)->globals;
}

/* Checks to see if the given container is immutable */
int _PyClassLoader_IsImmutable(PyObject* container);

#ifdef __cplusplus
}
#endif
#endif /* !Py_MODULEOBJECT_H */
