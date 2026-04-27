/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#ifndef Ci_CHECKED_DICT_H
#define Ci_CHECKED_DICT_H

#include "cinderx/python.h"

#include "cinderx/StaticPython/generic_type.h"

#ifdef __cplusplus
extern "C" {
#endif

extern PyTypeObject* Ci_CheckedDict_Type;
extern _PyGenericTypeDef Ci_CheckedDict_GenericType;

PyObject* Ci_CheckedDict_New(PyTypeObject* type);
PyObject* Ci_CheckedDict_NewPresized(PyTypeObject* type, Py_ssize_t minused);

int Ci_CheckedDict_Check(PyObject* x);
int Ci_CheckedDict_TypeCheck(PyTypeObject* type);

int Ci_CheckedDict_SetItem(PyObject* op, PyObject* key, PyObject* value);

int Ci_DictOrChecked_SetItem(PyObject* op, PyObject* key, PyObject* value);

void _PyCheckedDict_ClearCaches();

extern PyTypeObject Ci_CheckedDictItems_Type, Ci_CheckedDictValues_Type,
    Ci_CheckedDictKeys_Type;
extern PyTypeObject* Ci_CheckedDictIterKey_Type;
extern PyTypeObject* Ci_CheckedDictIterValue_Type;
extern PyTypeObject* Ci_CheckedDictIterItem_Type;
extern PyTypeObject* Ci_CheckedDictRevIterKey_Type;
extern PyTypeObject* Ci_CheckedDictRevIterItem_Type;
extern PyTypeObject* Ci_CheckedDictRevIterValue_Type;
extern PyType_Spec Ci_CheckedDictIterKey_Spec;
extern PyType_Spec Ci_CheckedDictIterValue_Spec;
extern PyType_Spec Ci_CheckedDictIterItem_Spec;
extern PyType_Spec Ci_CheckedDictRevIterKey_Spec;
extern PyType_Spec Ci_CheckedDictRevIterItem_Spec;
extern PyType_Spec Ci_CheckedDictRevIterValue_Spec;
#ifdef __cplusplus
}
#endif
#endif /* !Ci_CHECKED_DICT_H */
