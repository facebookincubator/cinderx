#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000

#include "pycore_typeobject.h"

#ifdef __cplusplus
extern "C" {
#endif

// genobject.c
PyObject* _PyGen_yf(PyGenObject *gen);
PyObject* _PyCoro_GetAwaitableIter(PyObject *o);
PyObject* _PyAsyncGenValueWrapperNew(PyThreadState *state, PyObject *);

// dictobject.c
int
_PyObjectDict_SetItem(PyTypeObject *tp, PyObject **dictptr,
                      PyObject *key, PyObject *value);
void
_PyDictKeys_DecRef(PyDictKeysObject *keys);
PyObject *
_PyDict_LoadGlobal(PyDictObject *globals, PyDictObject *builtins, PyObject *key);

// pycore_tuple.h
PyObject* _PyTuple_FromArray(PyObject *const *, Py_ssize_t);

// pycore_typeobject.h
static_builtin_state* _PyStaticType_GetState(PyInterpreterState *, PyTypeObject *);


#ifdef __cplusplus
} // extern "C"
#endif

#endif // PY_VERSION_HEX >= 0x030C0000
