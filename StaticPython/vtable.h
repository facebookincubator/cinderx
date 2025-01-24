/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#ifndef Ci_VTABLE_H
#define Ci_VTABLE_H

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
    Represents a V-table entrypoint (see below for what a V-table is).
*/
typedef struct {
  /* TODO: Would we better off with dynamically allocated stubs which close
   * over the function? */
  PyObject* vte_state;
  vectorcallfunc vte_entry;
} _PyType_VTableEntry;

typedef struct {
  // The generic type definition for this instantation
  PyObject* gtr_gtd;
  Py_ssize_t gtr_typeparam_count;
  // The type params for this instantiation
  PyTypeObject* gtr_typeparams[0];
} _PyType_GenericTypeRef;

/**
    This is the core datastructure used for efficient call dispatch at runtime.
    It is initialized lazily on Static types when a callable on any of them is
    called. It's stored as `tp_cache` on PyTypeObjects.
*/
typedef struct {
  /* Dict[str | tuple, int] - This contains a mapping of slot name to slot index
   */
  PyObject_VAR_HEAD PyObject* vt_slotmap;
  /* Dict[str | tuple, int] - This contains a mapping of slot name to original
     callables. This is used whenever patching comes into the picture. */
  PyObject* vt_original;
  /* Dict[str | tuple, Callable] A thunk is a wrapper over Python callables. We
    use them for a number of purposes, e.g: enforcing return type checks on
    patched functions */
  PyObject* vt_thunks;
  /* Dict[tuple[...], special_thunk] A special thunk is a wrapper around a
   * v-table slot for a getter or a setter, stored under the special name (name,
   * "fget"/"fset" )*/
  PyObject* vt_specials;
  /* Generic type instantiation info, used to manage the lifetime of the generic
   * type instantation parameters */
  _PyType_GenericTypeRef* vt_gtr;
  /* Size of the vtable */
  Py_ssize_t vt_size;
  int vt_typecode;
  _PyType_VTableEntry vt_entries[1];
} _PyType_VTable;

extern PyTypeObject _PyType_VTableType;

PyObject* _PyClassLoader_InvokeMethod(
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargsf);

#ifdef __cplusplus
}
#endif

#endif
