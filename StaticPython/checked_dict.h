/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#ifndef Ci_CHECKED_DICT_H
#define Ci_CHECKED_DICT_H

#include <Python.h>

// Hack to avoid having to add another dependency to the upgrade stubs.
// Once we are done with the 3.12 upgrade we can remove this.
#ifndef __UPGRADE_STUBS_CPP
#include "cinderx/StaticPython/classloader.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Hack to avoid having to add another dependency to the upgrade stubs.
// Once we are done with the 3.12 upgrade we can remove this.
#ifndef __UPGRADE_STUBS_CPP
extern _PyGenericTypeDef Ci_CheckedDict_Type;
#endif
PyObject * Ci_CheckedDict_New(PyTypeObject *type);
PyObject * Ci_CheckedDict_NewPresized(PyTypeObject *type, Py_ssize_t minused);

int Ci_CheckedDict_Check(PyObject *x);
int Ci_CheckedDict_TypeCheck(PyTypeObject *type);

int Ci_CheckedDict_SetItem(PyObject *op, PyObject *key, PyObject *value);

int Ci_DictOrChecked_SetItem(PyObject *op, PyObject *key, PyObject *value);

#ifdef __cplusplus
}
#endif
#endif /* !Ci_CHECKED_DICT_H */
