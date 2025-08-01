/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#ifndef Ci_CHECKED_LIST_H
#define Ci_CHECKED_LIST_H

#include "cinderx/python.h"

#include "cinderx/StaticPython/generic_type.h"

#ifdef __cplusplus
extern "C" {
#endif

PyObject* Ci_CheckedList_GetItem(PyObject* self, Py_ssize_t);
PyObject* Ci_CheckedList_New(PyTypeObject* type, Py_ssize_t);
int Ci_CheckedList_TypeCheck(PyTypeObject* type);
extern PyTypeObject* Ci_CheckedList_Type;
extern _PyGenericTypeDef Ci_CheckedList_GenericType;
int Ci_ListOrCheckedList_Append(PyListObject* self, PyObject* v);

int Ci_CheckedList_Check(PyObject* op);

void _PyCheckedList_ClearCaches();

#define Ci_CheckedList_CAST(op) \
  (assert(Ci_CheckedList_Check((PyObject*)op)), (PyListObject*)(op))

#define Ci_CheckedList_GET_ITEM(op, i) (Ci_CheckedList_CAST(op)->ob_item[i])
#define Ci_CheckedList_SET_ITEM(op, i, v) \
  ((void)(Ci_CheckedList_CAST(op)->ob_item[i] = (v)))
#define Ci_CheckedList_GET_SIZE(op) Py_SIZE(Ci_CheckedList_CAST(op))

#define Ci_ListOrCheckedList_GET_ITEM(op, i) (((PyListObject*)(op))->ob_item[i])
#define Ci_ListOrCheckedList_SET_ITEM(op, i, v) \
  ((void)(((PyListObject*)(op))->ob_item[i] = (v)))
#define Ci_ListOrCheckedList_GET_SIZE(op) Py_SIZE((PyListObject*)(op))

#ifdef __cplusplus
}
#endif
#endif /* !Ci_CHECKED_LIST_H */
