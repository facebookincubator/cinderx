// Copyright (c) Meta Platforms, Inc. and affiliates.

#ifndef Py_CACHED_PROPERTIES_H
#define Py_CACHED_PROPERTIES_H

#include "cinderx/python.h"

/* fb t46346203 */
typedef struct {
  PyObject_HEAD
  PyObject* func; /* function object */
  PyObject* name_or_descr; /* str or member descriptor object */
} PyCachedPropertyDescrObject;
/* end fb t46346203 */

/* fb T82701047 */
typedef struct {
  PyObject_HEAD
  PyObject* func; /* function object */
  PyObject* name_or_descr; /* str or member descriptor object */
} PyAsyncCachedPropertyDescrObject;
/* end fb T82701047 */

/* fb T82701047 */
typedef struct {
  PyObject_HEAD
  PyObject* func; /* function object */
  PyObject* name; /* str or member descriptor object */
  PyObject* value; /* value or NULL when uninitialized */
} PyAsyncCachedClassPropertyDescrObject;
/* end fb T82701047 */

extern PyTypeObject PyAsyncCachedPropertyWithDescr_Type;
extern PyType_Spec _PyCachedClassProperty_TypeSpec; /* fb t46346203 */
extern PyTypeObject PyCachedProperty_Type; /* fb T46346203 */
extern PyTypeObject PyCachedPropertyWithDescr_Type; /* fb T46346203 */
extern PyTypeObject PyAsyncCachedProperty_Type; /* fb T82701047 */
extern PyTypeObject PyAsyncCachedClassProperty_Type; /* fb T82701047 */

#endif /* !Py_CACHED_PROPERTIES_H */
