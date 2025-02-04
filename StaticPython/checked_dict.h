/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#ifndef Ci_CHECKED_DICT_H
#define Ci_CHECKED_DICT_H

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject* Ci_CheckedDict_Type;

PyObject* Ci_CheckedDict_New(PyTypeObject* type);
PyObject* Ci_CheckedDict_NewPresized(PyTypeObject* type, Py_ssize_t minused);

int Ci_CheckedDict_Check(PyObject* x);
int Ci_CheckedDict_TypeCheck(PyTypeObject* type);

int Ci_CheckedDict_SetItem(PyObject* op, PyObject* key, PyObject* value);

int Ci_DictOrChecked_SetItem(PyObject* op, PyObject* key, PyObject* value);

void _PyCheckedDict_ClearCaches();

#ifdef __cplusplus
}
#endif
#endif /* !Ci_CHECKED_DICT_H */
