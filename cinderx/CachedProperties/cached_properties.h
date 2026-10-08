// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

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

#ifdef __cplusplus
extern "C" {
#endif

extern PyType_Spec PyAsyncCachedClassProperty_Spec;
extern PyType_Spec PyAsyncCachedPropertyWithDescr_Spec;
extern PyType_Spec PyAsyncCachedProperty_Spec;
extern PyType_Spec PyCachedClassProperty_Spec;
extern PyType_Spec PyCachedPropertyWithDescr_Spec;
extern PyType_Spec PyCachedProperty_Spec;

#ifdef __cplusplus
} // extern "C"
#endif
