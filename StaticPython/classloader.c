/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/classloader.h"

#include <Python.h>

#include "descrobject.h"
#include "dictobject.h"
#include "object.h"
#include "pycore_object.h" // PyHeapType_CINDER_EXTRA
#include "pyerrors.h"
#include "pyport.h"
#include "structmember.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif
#include "cinderx/Upgrade/upgrade_stubs.h"  // @donotremove

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/extra-py-flags.h"  // @donotremove
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/entry.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/modulethunks.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_defs.h"

#include <dlfcn.h>

// This is a dict containing a mapping of lib name to "handle"
// as returned by `dlopen()`.
// Dict[str, int]
static PyObject* dlopen_cache;

// This is a dict containing a mapping of (lib_name, symbol_name) to
// the raw address as returned by `dlsym()`.
// Dict[Tuple[str, str], int]
static PyObject* dlsym_cache;

int used_in_vtable(PyObject* value);

extern int _PyObject_GetMethod(PyObject*, PyObject*, PyObject**);

static int rettype_check_traverse(
    _PyClassLoader_RetTypeInfo* op,
    visitproc visit,
    void* arg) {
  visit((PyObject*)op->rt_expected, arg);
  return 0;
}

static int rettype_check_clear(_PyClassLoader_RetTypeInfo* op) {
  Py_CLEAR(op->rt_expected);
  Py_CLEAR(op->rt_name);
  return 0;
}

/**
    This does the initialization of the vectorcall entrypoint for the v-table
   for static functions. It'll set the entrypoint to _PyVTable_func_lazyinit
   if the functions entry point hasn't yet been initialized.

    If it has been initialized and is being handled by the interpreter loop
   it'll go through the single Ci_PyFunction_CallStatic entry point. Otherwise
   it'll just use the function entry point, which should be JITed.
*/
// TODO: Drop name argument
static int _PyVTable_set_opt_slot(
    PyTypeObject* tp,
    PyObject* name,
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject* value) {
  vectorcallfunc entry = ((PyFunctionObject*)value)->vectorcall;
  if (entry == (vectorcallfunc)Ci_JIT_lazyJITInitFuncObjectVectorcall) {
    /* entry point isn't initialized yet, we want to run it once, and
     * then update our own entry point */
    int optional, exact, func_flags;
    PyTypeObject* ret_type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
        value, &optional, &exact, &func_flags);
    int type_code = _PyClassLoader_GetTypeCode(ret_type);

    PyObject* state = PyTuple_New(type_code != TYPED_OBJECT ? 4 : 3);
    if (state == NULL) {
      return -1;
    }
    PyTuple_SET_ITEM(state, 0, (PyObject*)vtable);
    Py_INCREF(vtable);
    PyObject* new_index = PyLong_FromSize_t(slot);
    if (new_index == NULL) {
      Py_DECREF(state);
      return -1;
    }
    PyTuple_SET_ITEM(state, 1, new_index);
    PyTuple_SET_ITEM(state, 2, value);
    if (type_code != TYPED_OBJECT) {
      PyTuple_SET_ITEM(state, 3, (PyObject*)ret_type);
      Py_INCREF(ret_type);
    }
    Py_INCREF(value);
    Py_XDECREF(vtable->vt_entries[slot].vte_state);
    vtable->vt_entries[slot].vte_state = state;
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_func_lazyinit_dont_bolt;
  } else if (entry == (vectorcallfunc)_PyFunction_Vectorcall) {
    // non-JITed function, it could return a primitive in which case we need a
    // stub to unbox the value.
    int optional, exact, func_flags;
    PyTypeObject* ret_type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
        value, &optional, &exact, &func_flags);
    int type_code = _PyClassLoader_GetTypeCode(ret_type);

    if (type_code != TYPED_OBJECT) {
      PyObject* tuple = PyTuple_New(2);
      if (tuple == NULL) {
        return -1;
      }
      PyTuple_SET_ITEM(tuple, 0, value);
      Py_INCREF(value);
      PyTuple_SET_ITEM(tuple, 1, (PyObject*)ret_type);
      Py_INCREF(ret_type);
      vtable->vt_entries[slot].vte_state = tuple;
      vtable->vt_entries[slot].vte_entry =
          (vectorcallfunc)_PyVTable_thunk_ret_primitive_not_jitted_dont_bolt;
    } else {
      Py_XDECREF(vtable->vt_entries[slot].vte_state);
      vtable->vt_entries[slot].vte_state = value;
      vtable->vt_entries[slot].vte_entry = _PyClassLoader_GetStaticFunctionEntry((PyFunctionObject*)value);
      Py_INCREF(value);
    }
  } else {
    Py_XDECREF(vtable->vt_entries[slot].vte_state);
    vtable->vt_entries[slot].vte_state = value;
    vtable->vt_entries[slot].vte_entry = _PyClassLoader_GetStaticFunctionEntry((PyFunctionObject*)value);
    Py_INCREF(value);
  }
  return 0;
}


static PyObject* g_missing_fget = NULL;
static PyObject* g_missing_fset = NULL;

static PyObject* classloader_get_property_missing_fget() {
  if (g_missing_fget == NULL) {
    PyObject* mod = PyImport_ImportModule("_static");
    if (mod == NULL) {
      return NULL;
    }
    PyObject* func = PyObject_GetAttrString(mod, "_property_missing_fget");
    Py_DECREF(mod);
    if (func == NULL) {
      return NULL;
    }
    g_missing_fget = func;
  }
  return g_missing_fget;
}

static PyObject* classloader_get_property_missing_fset() {
  if (g_missing_fset == NULL) {
    PyObject* mod = PyImport_ImportModule("_static");
    if (mod == NULL) {
      return NULL;
    }
    PyObject* func = PyObject_GetAttrString(mod, "_property_missing_fset");
    Py_DECREF(mod);
    if (func == NULL) {
      return NULL;
    }
    g_missing_fset = func;
  }
  return g_missing_fset;
}

static PyObject* classloader_ensure_specials_cache(PyTypeObject* type) {
  _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(type, 0);
  if (vtable == NULL) {
    return NULL;
  }
  PyObject* specials = vtable->vt_specials;
  if (specials == NULL) {
    specials = vtable->vt_specials = PyDict_New();
    if (specials == NULL) {
      return NULL;
    }
  }

  return specials;
}

/* Stores a newly created special thunk in the special thunk cache.  If it fails
 * to store decref the thunk and return NULL */
static PyObject* classloader_cache_new_special(
    PyTypeObject* type,
    PyObject* name,
    PyObject* special) {
  if (type == NULL) {
    return special;
  }
  PyObject* specials = classloader_ensure_specials_cache(type);
  if (specials == NULL) {
    return NULL;
  }

  if (PyDict_SetItem(specials, name, special)) {
    Py_DECREF(special);
    return NULL;
  }
  return special;
}

static PyObject* classloader_get_property_fget(
    PyTypeObject* type,
    PyObject* name,
    PyObject* property) {
  if (Py_TYPE(property) == &PyProperty_Type) {
    PyObject* func = ((Ci_propertyobject*)property)->prop_get;
    if (func == NULL) {
      func = classloader_get_property_missing_fget();
    }
    Py_XINCREF(func);
    return func;
  } else if (Py_TYPE(property) == &PyCachedPropertyWithDescr_Type) {
    _Py_CachedPropertyThunk* thunk = _Py_CachedPropertyThunk_New(property);
    if (thunk == NULL) {
      return NULL;
    }

    return classloader_cache_new_special(type, name, (PyObject*)thunk);
  } else if (Py_TYPE(property) == &PyAsyncCachedPropertyWithDescr_Type) {
    _Py_AsyncCachedPropertyThunk* thunk = _Py_AsyncCachedPropertyThunk_New(property);
    if (thunk == NULL) {
      return NULL;
    }

    return classloader_cache_new_special(type, name, (PyObject*)thunk);
  } else if (Py_TYPE(property) == &_PyTypedDescriptorWithDefaultValue_Type) {
    PyObject *thunk = _PyClassLoader_TypedDescriptorThunkGet_New(property);
    if (thunk == NULL) {
      return NULL;
    }
    return classloader_cache_new_special(type, name, thunk);
  } else {
    PyObject *thunk = _PyClassLoader_PropertyThunkGet_New(property);
    if (thunk == NULL) {
      return NULL;
    }
    return classloader_cache_new_special(type, name, thunk);
  }
}

static PyObject* classloader_get_property_fset(
    PyTypeObject* type,
    PyObject* name,
    PyObject* property) {
  if (Py_TYPE(property) == &PyProperty_Type) {
    PyObject* func = ((Ci_propertyobject*)property)->prop_set;
    if (func == NULL) {
      func = classloader_get_property_missing_fset();
    }
    Py_XINCREF(func);
    return func;
  } else if (
      Py_TYPE(property) == &PyCachedPropertyWithDescr_Type ||
      Py_TYPE(property) == &PyAsyncCachedPropertyWithDescr_Type) {
    PyObject* func = classloader_get_property_missing_fset();
    Py_XINCREF(func);
    return func;
  } else if (Py_TYPE(property) == &_PyTypedDescriptorWithDefaultValue_Type) {
    PyObject *thunk = _PyClassLoader_TypedDescriptorThunkSet_New(property);
    if (thunk == NULL) {
      return NULL;
    }
    return classloader_cache_new_special(type, name, thunk);
  } else {
    PyObject *thunk = _PyClassLoader_PropertyThunkSet_New(property);
    if (thunk == NULL) {
      return NULL;
    }
    return classloader_cache_new_special(type, name, thunk);
  }
}

static PyObject* classloader_get_property_method(
    PyTypeObject* type,
    PyObject* property,
    PyTupleObject* name) {
  PyObject* fname = PyTuple_GET_ITEM(name, 1);
  if (_PyUnicode_EqualToASCIIString(fname, "fget")) {
    return classloader_get_property_fget(type, (PyObject*)name, property);
  } else if (_PyUnicode_EqualToASCIIString(fname, "fset")) {
    return classloader_get_property_fset(type, (PyObject*)name, property);
  }
  PyErr_Format(
      PyExc_RuntimeError, "bad property method name %R in classloader", fname);
  return NULL;
}

static PyTypeObject* resolve_function_rettype(
    PyObject* funcobj,
    int* optional,
    int* exact,
    int* func_flags) {
  assert(PyFunction_Check(funcobj));
  PyFunctionObject* func = (PyFunctionObject*)funcobj;
  if (((PyCodeObject*)func->func_code)->co_flags & CO_COROUTINE) {
    *func_flags |= Ci_FUNC_FLAGS_COROUTINE;
  }
  return _PyClassLoader_ResolveType(
      _PyClassLoader_GetReturnTypeDescr(func), optional, exact);
}

static int _PyClassLoader_TypeCheckState_traverse(
    _PyClassLoader_TypeCheckState* op,
    visitproc visit,
    void* arg) {
  rettype_check_traverse((_PyClassLoader_RetTypeInfo*)op, visit, arg);
  visit(op->tcs_value, arg);
  visit(op->tcs_rt.rt_base.mt_original, arg);
  return 0;
}

static int _PyClassLoader_TypeCheckState_clear(
    _PyClassLoader_TypeCheckState* op) {
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_CLEAR(op->tcs_value);
  Py_CLEAR(op->tcs_rt.rt_base.mt_original);
  return 0;
}

static void _PyClassLoader_TypeCheckState_dealloc(
    _PyClassLoader_TypeCheckState* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_XDECREF(op->tcs_value);
  Py_XDECREF(op->tcs_rt.rt_base.mt_original);
  PyObject_GC_Del((PyObject*)op);
}

PyTypeObject _PyType_TypeCheckState = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable_state_obj",
    sizeof(_PyClassLoader_TypeCheckState),
    .tp_dealloc = (destructor)_PyClassLoader_TypeCheckState_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_traverse = (traverseproc)_PyClassLoader_TypeCheckState_traverse,
    .tp_clear = (inquiry)_PyClassLoader_TypeCheckState_clear,
};

static void _PyClassLoader_MethodThunk_dealloc(_PyClassLoader_MethodThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  Py_XDECREF(op->mt_original);
  PyObject_GC_Del((PyObject*)op);
}

static int _PyClassLoader_MethodThunk_traverse(
    _PyClassLoader_MethodThunk* op,
    visitproc visit,
    void* arg) {
  visit(op->mt_original, arg);
  return 0;
}

static int _PyClassLoader_MethodThunk_clear(_PyClassLoader_MethodThunk* op) {
  Py_CLEAR(op->mt_original);
  return 0;
}

PyTypeObject _PyType_MethodThunk = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "vtable_method_thunk",
    sizeof(_PyClassLoader_MethodThunk),
    .tp_dealloc = (destructor)_PyClassLoader_MethodThunk_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,
    .tp_traverse = (traverseproc)_PyClassLoader_MethodThunk_traverse,
    .tp_clear = (inquiry)_PyClassLoader_MethodThunk_clear,
};

static int _PyVTable_setslot_typecheck(
    PyTypeObject* decltype,
    PyObject* ret_type,
    int optional,
    int exact,
    int func_flags,
    PyObject* name,
    _PyType_VTable* vtable,
    Py_ssize_t slot,
    PyObject* value,
    PyObject* original) {
  _PyClassLoader_TypeCheckState* state =
      PyObject_GC_New(_PyClassLoader_TypeCheckState, &_PyType_TypeCheckState);
  if (state == NULL) {
    return -1;
  }
  state->tcs_value = value;
  Py_INCREF(value);
  state->tcs_rt.rt_name = name;
  Py_INCREF(name);
  state->tcs_rt.rt_expected = (PyTypeObject*)ret_type;
  Py_INCREF(ret_type);
  state->tcs_rt.rt_optional = optional;
  state->tcs_rt.rt_exact = exact;
  state->tcs_rt.rt_base.mt_original = original;
  Py_INCREF(original);

  Py_XDECREF(vtable->vt_entries[slot].vte_state);
  vtable->vt_entries[slot].vte_state = (PyObject*)state;
  if (func_flags & Ci_FUNC_FLAGS_COROUTINE) {
    if (func_flags & Ci_FUNC_FLAGS_CLASSMETHOD) {
      PyObject* tuple = PyTuple_New(2);
      if (tuple == NULL) {
        Py_DECREF(state);
        return -1;
      }
      PyTuple_SET_ITEM(tuple, 0, value);
      PyTuple_SET_ITEM(tuple, 1, (PyObject*)decltype);
      Py_INCREF(decltype);
      state->tcs_value = tuple;
      vtable->vt_entries[slot].vte_entry =
          (vectorcallfunc)_PyVTable_coroutine_classmethod_dont_bolt;
    } else if (
        PyTuple_Check(name) &&
        _PyClassLoader_IsPropertyName((PyTupleObject*)name)) {
      vtable->vt_entries[slot].vte_entry =
          (vectorcallfunc)_PyVTable_coroutine_property_dont_bolt;
    } else {
      vtable->vt_entries[slot].vte_entry =
          (vectorcallfunc)_PyVTable_coroutine_dont_bolt;
    }
  } else if (
      PyTuple_Check(name) &&
      _PyClassLoader_IsPropertyName((PyTupleObject*)name)) {
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_nonfunc_property_dont_bolt;
  } else if (PyFunction_Check(value)) {
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_func_overridable_dont_bolt;
  } else if (func_flags & Ci_FUNC_FLAGS_CLASSMETHOD) {
    PyObject* tuple = PyTuple_New(2);
    if (tuple == NULL) {
      Py_DECREF(state);
      return -1;
    }
    PyTuple_SET_ITEM(tuple, 0, value);
    PyTuple_SET_ITEM(tuple, 1, (PyObject*)decltype);
    Py_INCREF(decltype);
    state->tcs_value = tuple;
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_classmethod_overridable_dont_bolt;
  } else {
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_nonfunc_dont_bolt;
  }
  return 0;
}

/**
    As the name suggests, this creates v-tables for all subclasses of the given
   type (recursively).
*/
static int type_init_subclass_vtables(PyTypeObject* target_type) {
  /* TODO: This can probably be a lot more efficient.  If a type
   * hasn't been fully loaded yet we can probably propagate the
   * parent dict down, and either initialize the slot to the parent
   * slot (if not overridden) or initialize the slot to the child slot.
   * We then only need to populate the child dict w/ its members when
   * a member is accessed from the child type.  When we init the child
   * we can check if it's dict sharing with its parent. */
  PyObject* ref;
  PyObject* subclasses = target_type->tp_subclasses;
  if (subclasses != NULL) {
    Py_ssize_t i = 0;
    while (PyDict_Next(subclasses, &i, NULL, &ref)) {
      assert(PyWeakref_CheckRef(ref));
      ref = PyWeakref_GET_OBJECT(ref);
      if (ref == Py_None) {
        continue;
      }

      PyTypeObject* subtype = (PyTypeObject*)ref;
      if (subtype->tp_cache != NULL) {
        /* already inited */
        continue;
      }

      _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(subtype, 1);
      if (vtable == NULL) {
        return -1;
      }
    }
  }
  return 0;
}

static void _PyClassLoader_UpdateDerivedSlot(
    PyTypeObject* type,
    PyObject* name,
    Py_ssize_t index,
    PyObject* state,
    vectorcallfunc func) {
  /* Update any derived types which don't have slots */
  PyObject* ref;
  PyObject* subclasses = type->tp_subclasses;
  if (subclasses != NULL) {
    Py_ssize_t i = 0;
    while (PyDict_Next(subclasses, &i, NULL, &ref)) {
      assert(PyWeakref_CheckRef(ref));
      ref = PyWeakref_GET_OBJECT(ref);
      if (ref == Py_None) {
        continue;
      }

      PyTypeObject* subtype = (PyTypeObject*)ref;
      PyObject* override = PyDict_GetItem(_PyType_GetDict(subtype), name);
      if (override != NULL) {
        /* subtype overrides the value */
        continue;
      }

      assert(subtype->tp_cache != NULL);
      _PyType_VTable* subvtable = (_PyType_VTable*)subtype->tp_cache;
      Py_XDECREF(subvtable->vt_entries[index].vte_state);
      subvtable->vt_entries[index].vte_state = state;
      Py_INCREF(state);
      subvtable->vt_entries[index].vte_entry = func;

      _PyClassLoader_UpdateDerivedSlot(subtype, name, index, state, func);
    }
  }
}

static int thunktraverse(_Py_StaticThunk* op, visitproc visit, void* arg) {
  rettype_check_traverse((_PyClassLoader_RetTypeInfo*)op, visit, arg);
  Py_VISIT(op->thunk_tcs.tcs_value);
  Py_VISIT((PyObject*)op->thunk_cls);
  return 0;
}

static int thunkclear(_Py_StaticThunk* op) {
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_CLEAR(op->thunk_tcs.tcs_value);
  Py_CLEAR(op->thunk_cls);
  return 0;
}

static void thunkdealloc(_Py_StaticThunk* op) {
  PyObject_GC_UnTrack((PyObject*)op);
  rettype_check_clear((_PyClassLoader_RetTypeInfo*)op);
  Py_XDECREF(op->thunk_tcs.tcs_value);
  Py_XDECREF(op->thunk_cls);
  PyObject_GC_Del((PyObject*)op);
}

int get_func_or_special_callable(
    PyTypeObject* type,
    PyObject* name,
    PyObject** result);

int _PyClassLoader_InitTypeForPatching(PyTypeObject* type) {
  if (!(type->tp_flags & Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED)) {
    return 0;
  }
  _PyType_VTable* vtable = (_PyType_VTable*)type->tp_cache;
  if (vtable != NULL && vtable->vt_original != NULL) {
    return 0;
  }
  if (_PyClassLoader_EnsureVtable(type, 0) == NULL) {
    return -1;
  }
  vtable = (_PyType_VTable*)type->tp_cache;

  PyObject *name, *slot, *clsitem;
  PyObject* slotmap = vtable->vt_slotmap;
  PyObject* origitems = vtable->vt_original = PyDict_New();

  Py_ssize_t i = 0;
  while (PyDict_Next(slotmap, &i, &name, &slot)) {
    if (get_func_or_special_callable(type, name, &clsitem)) {
      return -1;
    }
    if (clsitem != NULL) {
      if (PyDict_SetItem(origitems, name, clsitem)) {
        Py_DECREF(clsitem);
        goto error;
      }
      Py_DECREF(clsitem);
    }
  }
  return 0;
error:
  vtable->vt_original = NULL;
  Py_DECREF(origitems);
  return -1;
}

int get_func_or_special_callable(
    PyTypeObject* type,
    PyObject* name,
    PyObject** result) {
  PyObject* dict = _PyType_GetDict(type);
  if (PyTuple_CheckExact(name)) {
    if (_PyClassLoader_IsPropertyName((PyTupleObject*)name)) {
      _PyType_VTable* vtable = (_PyType_VTable*)type->tp_cache;
      if (vtable != NULL) {
        PyObject* specials = vtable->vt_specials;
        if (specials != NULL) {
          *result = PyDict_GetItem(specials, name);
          if (*result != NULL) {
            Py_INCREF(*result);
            return 0;
          }
        }
      }

      PyObject* property = PyDict_GetItem(dict, PyTuple_GET_ITEM(name, 0));
      if (property == NULL) {
        *result = NULL;
        return 0;
      }
      *result =
          classloader_get_property_method(type, property, (PyTupleObject*)name);
      if (*result == NULL) {
        return -1;
      }
      return 0;
    }
  }
  *result = PyDict_GetItem(dict, name);
  Py_XINCREF(*result);
  return 0;
}

int _PyClassLoader_IsPatchedThunk(PyObject* obj) {
  if (obj != NULL && Py_TYPE(obj) == &_PyType_StaticThunk) {
    return 1;
  }
  return 0;
}

int is_static_type(PyTypeObject* type);

/*
    Looks up through parent classes to find a member specified by the name. If a
   parent class attribute has been patched, that is ignored, i.e it goes through
   the originally defined members.
*/
int _PyClassLoader_GetStaticallyInheritedMember(
    PyTypeObject* type,
    PyObject* name,
    PyObject** result) {
  PyObject *mro = type->tp_mro, *base;

  for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); i++) {
    PyTypeObject* next = (PyTypeObject*)PyTuple_GET_ITEM(type->tp_mro, i);
    if (!is_static_type(next)) {
      continue;
    }
    if (next->tp_cache != NULL &&
        ((_PyType_VTable*)next->tp_cache)->vt_original != NULL) {
      /* if we've initialized originals it contains all of our possible slot
       * values including special callables. */
      base =
          PyDict_GetItem(((_PyType_VTable*)next->tp_cache)->vt_original, name);
      if (base == NULL) {
        continue;
      }
      assert(used_in_vtable(base));
      Py_INCREF(base);
      *result = base;
      return 0;
    } else if (_PyType_GetDict(next) == NULL) {
      continue;
    } else if (get_func_or_special_callable(next, name, &base)) {
      return -1;
    }

    if (base != NULL) {
      *result = base;
      return 0;
    }
  }
  *result = NULL;
  return 0;
}

static PyObject* g_fget = NULL;
static PyObject* g_fset = NULL;

PyObject* get_descr_tuple(PyObject* name, PyObject* accessor) {
  PyObject* getter_tuple = PyTuple_New(2);
  Py_INCREF(name);
  PyTuple_SET_ITEM(getter_tuple, 0, name);
  Py_INCREF(accessor);
  PyTuple_SET_ITEM(getter_tuple, 1, accessor);
  return getter_tuple;
}

PyObject* get_property_getter_descr_tuple(PyObject* name) {
  if (g_fget == NULL) {
    g_fget = PyUnicode_FromStringAndSize("fget", 4);
  }
  return get_descr_tuple(name, g_fget);
}

PyObject* get_property_setter_descr_tuple(PyObject* name) {
  if (g_fset == NULL) {
    g_fset = PyUnicode_FromStringAndSize("fset", 4);
  }
  return get_descr_tuple(name, g_fset);
}

/* Static types have a slot containing all final methods in their inheritance
   chain. This function returns the contents of that slot by looking up the MRO,
   if it exists.
 */
static PyObject* get_final_method_names(PyTypeObject* type) {
  PyObject* mro = type->tp_mro;
  if (mro == NULL) {
    return NULL;
  }
  Py_ssize_t n = PyTuple_GET_SIZE(mro);
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject* mro_type = PyTuple_GET_ITEM(mro, i);
    if (((PyTypeObject*)mro_type)->tp_flags &
        Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED) {
      _Py_IDENTIFIER(__final_method_names__);
      PyObject* final_method_names_string =
          _PyUnicode_FromId(&PyId___final_method_names__);
      PyObject* final_method_names = _PyObject_GenericGetAttrWithDict(
          mro_type,
          final_method_names_string,
          /*dict=*/NULL,
          /*suppress=*/1);
      return final_method_names;
    }
  }
  return NULL;
}

int _PyClassLoader_IsFinalMethodOverridden(
    PyTypeObject* base_type,
    PyObject* members_dict) {
  PyObject* final_method_names = get_final_method_names(base_type);
  if (final_method_names == NULL) {
    return 0;
  }
  if (!PyTuple_Check(final_method_names)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "The __final_method_names__ slot for type %R is not a tuple.",
        final_method_names);
    Py_DECREF(final_method_names);
    return -1;
  }
  Py_ssize_t member_pos = 0;
  PyObject *key, *value;
  while (PyDict_Next(members_dict, &member_pos, &key, &value)) {
    for (Py_ssize_t final_method_index = 0;
         final_method_index < PyTuple_GET_SIZE(final_method_names);
         final_method_index++) {
      PyObject* current_final_method_name =
          PyTuple_GET_ITEM(final_method_names, final_method_index);
      int compare_result = PyUnicode_Compare(key, current_final_method_name);
      if (compare_result == 0) {
        PyErr_Format(
            CiExc_StaticTypeError,
            "%R overrides a final method in the static base class %R",
            key,
            base_type);
        Py_DECREF(final_method_names);
        return -1;
      } else if (compare_result == -1 && PyErr_Occurred()) {
        return -1;
      }
    }
  }
  Py_DECREF(final_method_names);
  return 0;
}

static int check_if_final_method_overridden(
    PyTypeObject* type,
    PyObject* name) {
  PyTypeObject* base_type = type->tp_base;
  if (base_type == NULL) {
    return 0;
  }
  PyObject* final_method_names = get_final_method_names(base_type);
  if (final_method_names == NULL) {
    return 0;
  }
  if (!PyTuple_Check(final_method_names)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "The __final_method_names__ slot for type %R is not a tuple.",
        final_method_names);
    Py_DECREF(final_method_names);
    return -1;
  }
  for (Py_ssize_t final_method_index = 0;
       final_method_index < PyTuple_GET_SIZE(final_method_names);
       final_method_index++) {
    PyObject* current_final_method_name =
        PyTuple_GET_ITEM(final_method_names, final_method_index);
    int compare_result = PyUnicode_Compare(name, current_final_method_name);
    if (compare_result == 0) {
      PyErr_Format(
          CiExc_StaticTypeError,
          "%R overrides a final method in the static base class %R",
          name,
          base_type);
      Py_DECREF(final_method_names);
      return -1;
    } else if (compare_result == -1 && PyErr_Occurred()) {
      Py_DECREF(final_method_names);
      return -1;
    }
  }
  Py_DECREF(final_method_names);
  return 0;
}

int populate_getter_and_setter(
    PyTypeObject* type,
    PyObject* name,
    PyObject* new_value) {
  PyObject* getter_value = new_value == NULL
      ? NULL
      : classloader_get_property_fget(type, name, new_value);
  PyObject* setter_value = new_value == NULL
      ? NULL
      : classloader_get_property_fset(type, name, new_value);

  PyObject* getter_tuple = get_property_getter_descr_tuple(name);
  PyObject* setter_tuple = get_property_setter_descr_tuple(name);

  int result = 0;
  if (_PyClassLoader_UpdateSlot(type, (PyObject*)getter_tuple, getter_value)) {
    result = -1;
  }
  Py_DECREF(getter_tuple);
  Py_XDECREF(getter_value);

  if (_PyClassLoader_UpdateSlot(type, (PyObject*)setter_tuple, setter_value)) {
    result = -1;
  }
  Py_DECREF(setter_tuple);
  Py_XDECREF(setter_value);

  return result;
}

static int classloader_get_original_static_def(
    PyTypeObject* tp,
    PyObject* name,
    PyObject** original) {
  _PyType_VTable* vtable = (_PyType_VTable*)tp->tp_cache;
  *original = NULL;
  if (is_static_type(tp)) {
    if (vtable->vt_original != NULL) {
      *original = PyDict_GetItem(vtable->vt_original, name);
      if (*original != NULL) {
        Py_INCREF(*original);
        return 0;
      }
    } else if (get_func_or_special_callable(tp, name, original)) {
      return -1;
    }
    // If a static type has a non-static member (for instance, due to having a
    // decorated method) we need to keep looking up the MRO for a static base.
    if (*original == NULL || !used_in_vtable(*original)) {
      Py_CLEAR(*original);
    }
  }

  if (*original == NULL) {
    // The member was actually defined in one of the parent classes, so try to
    // look it up from there.
    // TODO: It might be possible to avoid the type-check in this situation,
    // because while `tp` was patched, the parent Static classes may not be.
    if (_PyClassLoader_GetStaticallyInheritedMember(tp, name, original)) {
      return -1;
    }
  }
  return 0;
}

static int _PyVTable_setslot(
    PyTypeObject* tp,
    PyObject* name,
    Py_ssize_t slot,
    PyObject* value,
    PyObject* original);

/* The UpdateSlot method will always get called by `tp_setattro` when one of a
   type's attribute gets changed, and serves as an entry point for handling
   modifications to vtables. */
int _PyClassLoader_UpdateSlot(
    PyTypeObject* type,
    PyObject* name,
    PyObject* new_value) {
  /* This check needs to be happen before we look into the vtable, as non-static
     subclasses of static classes won't necessarily have vtables already
     constructed. */
  if (check_if_final_method_overridden(type, name)) {
    return -1;
  }
  _PyType_VTable* vtable = (_PyType_VTable*)type->tp_cache;
  if (vtable == NULL) {
    return 0;
  }

  PyObject* slotmap = vtable->vt_slotmap;
  PyObject* slot = PyDict_GetItem(slotmap, name);
  if (slot == NULL) {
    return 0;
  }

  PyObject* original;
  if (classloader_get_original_static_def(type, name, &original)) {
    return -1;
  }

  /* we need to search in the MRO if we don't contain the
   * item directly or we're currently deleting the current value */
  if (new_value == NULL) {
    /* We need to look for an item explicitly declared in our parent if we're
     * inheriting. Note we don't care about static vs non-static, and we don't
     * want to look at the original values either.  The new value is simply
     * whatever the currently inherited value is. */
    PyObject* mro = type->tp_mro;

    for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(mro); i++) {
      PyTypeObject* next = (PyTypeObject*)PyTuple_GET_ITEM(type->tp_mro, i);
      PyObject* next_dict = _PyType_GetDict(next);
      if (next_dict == NULL) {
        continue;
      }
      new_value = PyDict_GetItem(next_dict, name);
      if (new_value != NULL) {
        break;
      }
    }
  }

  /* update the value that exists in our thunks for performing indirections
   * necessary for patched INVOKE_FUNCTION calls */
  if (vtable->vt_thunks != NULL) {
    _Py_StaticThunk* thunk =
        (_Py_StaticThunk*)PyDict_GetItem(vtable->vt_thunks, name);
    if (thunk != NULL) {
      _PyClassLoader_UpdateThunk(thunk, original, new_value);
    }
  }

  assert(original != NULL);

  int cur_optional = 0, cur_exact = 0, cur_func_flags = 0;
  PyObject* cur_type = _PyClassLoader_ResolveReturnType(
      original, &cur_optional, &cur_exact, &cur_func_flags);
  assert(cur_type != NULL);

  // if this is a property slot, also update the getter and setter slots
  if (Py_TYPE(original) == &PyProperty_Type ||
      Py_TYPE(original) == &PyCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &PyAsyncCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &_PyTypedDescriptorWithDefaultValue_Type) {
    if (new_value) {
      // If we have a new value, and it's not a descriptor, we can type-check it
      // at the time of assignment.
      PyTypeObject* new_value_type = Py_TYPE(new_value);
      if (new_value_type->tp_descr_get == NULL &&
          !_PyObject_TypeCheckOptional(
              new_value, (PyTypeObject*)cur_type, cur_optional, cur_exact)) {
        PyErr_Format(
            CiExc_StaticTypeError,
            "Cannot assign a %s, because %s.%U is expected to be a %s",
            Py_TYPE(new_value)->tp_name,
            type->tp_name,
            name,
            ((PyTypeObject*)cur_type)->tp_name);
        Py_DECREF(cur_type);
        Py_DECREF(original);
        return -1;
      }
    }
    if (populate_getter_and_setter(type, name, new_value) < 0) {
      Py_DECREF(original);
      return -1;
    }
  }
  Py_DECREF(cur_type);

  Py_ssize_t index = PyLong_AsSsize_t(slot);

  if (_PyVTable_setslot(type, name, index, new_value, original)) {
    Py_DECREF(original);
    return -1;
  }

  Py_DECREF(original);

  /* propagate slot update to derived classes that don't override
   * the function (but first, ensure they have initialized vtables) */
  if (type_init_subclass_vtables(type) != 0) {
    return -1;
  }
  _PyClassLoader_UpdateDerivedSlot(
      type,
      name,
      index,
      vtable->vt_entries[index].vte_state,
      vtable->vt_entries[index].vte_entry);
  return 0;
}

/**
    Sets the vtable slot entry for the given method name to the correct type of
   vectorcall. We specialize where possible, but also have a generic fallback
   which checks whether the actual return type matches the declared one (if
   any).
*/
static int _PyVTable_setslot(
    PyTypeObject* tp,
    PyObject* name,
    Py_ssize_t slot,
    PyObject* value,
    PyObject* original) {
  _PyType_VTable* vtable = (_PyType_VTable*)tp->tp_cache;
  assert(original != NULL);

  if (original == value) {
    if (tp->tp_dictoffset == 0) {
      // These cases mean that the type instances don't have a __dict__ slot,
      // meaning our compile time type-checks are valid (nothing's been patched)
      // meaning we can omit return type checks at runtime.
      if (_PyClassLoader_IsStaticFunction(value)) {
        return _PyVTable_set_opt_slot(tp, name, vtable, slot, value);
      } else if (
          Py_TYPE(value) == &PyStaticMethod_Type &&
          _PyClassLoader_IsStaticFunction(Ci_PyStaticMethod_GetFunc(value))) {
        Py_XSETREF(vtable->vt_entries[slot].vte_state, value);
        vtable->vt_entries[slot].vte_entry =
            (vectorcallfunc)&_PyVTable_staticmethod_dont_bolt;
        Py_INCREF(value);
        return 0;
      } else if (
          Py_TYPE(value) == &PyClassMethod_Type &&
          _PyClassLoader_IsStaticFunction(Ci_PyClassMethod_GetFunc(value))) {
        PyObject* tuple = PyTuple_New(2);
        if (tuple == NULL) {
          return -1;
        }
        PyTuple_SET_ITEM(tuple, 0, value);
        PyTuple_SET_ITEM(tuple, 1, (PyObject*)tp);
        Py_INCREF(tp);
        Py_XSETREF(vtable->vt_entries[slot].vte_state, tuple);
        vtable->vt_entries[slot].vte_entry =
            (vectorcallfunc)_PyVTable_classmethod_dont_bolt;
        Py_INCREF(value);
        return 0;
      } else if (Py_TYPE(value) == &PyMethodDescr_Type) {
        Py_XSETREF(vtable->vt_entries[slot].vte_state, value);
        vtable->vt_entries[slot].vte_entry =
            (vectorcallfunc)&_PyVTable_thunk_vectorcall_only_dont_bolt;
        Py_INCREF(value);
        return 0;
      }
    }

    if (Py_TYPE(value) == &_PyType_CachedPropertyThunk ||
        Py_TYPE(value) == &_PyType_TypedDescriptorThunk) {
      Py_XSETREF(vtable->vt_entries[slot].vte_state, value);
      vtable->vt_entries[slot].vte_entry =
          (vectorcallfunc)_PyVTable_descr_dont_bolt;
      Py_INCREF(value);
      return 0;
    }
  }

  int optional = 0, exact = 0, func_flags = 0;
  PyObject* ret_type = _PyClassLoader_ResolveReturnType(
      original, &optional, &exact, &func_flags);

  if (ret_type == NULL) {
    PyErr_Format(
        PyExc_RuntimeError,
        "missing type annotation on static compiled method %R of %s",
        name,
        tp->tp_name);
    return -1;
  }

  if (value == NULL) {
    PyObject* missing_state = PyTuple_New(4);
    if (missing_state == NULL) {
      Py_DECREF(ret_type);
      return -1;
    }

    PyObject* func_name = _PyClassLoader_GetFunctionName(name);
    PyTuple_SET_ITEM(missing_state, 0, func_name);
    PyTuple_SET_ITEM(missing_state, 1, (PyObject*)tp);
    PyObject* optional_obj = optional ? Py_True : Py_False;
    PyTuple_SET_ITEM(missing_state, 2, optional_obj);
    PyTuple_SET_ITEM(missing_state, 3, original);
    Py_INCREF(func_name);
    Py_INCREF(tp);
    Py_INCREF(optional_obj);
    Py_INCREF(original);

    Py_XDECREF(vtable->vt_entries[slot].vte_state);
    vtable->vt_entries[slot].vte_state = missing_state;
    vtable->vt_entries[slot].vte_entry =
        (vectorcallfunc)_PyVTable_func_missing_dont_bolt;
    Py_DECREF(ret_type);
    return 0;
  }

  int res = _PyVTable_setslot_typecheck(
      tp,
      ret_type,
      optional,
      exact,
      func_flags,
      name,
      vtable,
      slot,
      value,
      original);
  Py_DECREF(ret_type);
  return res;
}

/**
    This is usually what we use as the initial entrypoint in v-tables. Then,
    when a method is called, this traverses the MRO, finds the correct callable,
    and updates the vtable entry with the correct one (and then calls the
    callable). All following method invokes directly hit the actual callable,
    because the v-table has been updated.
*/
static _PyClassLoader_StaticCallReturn _PyVTable_lazyinit_impl(
    PyObject* info,
    void** args,
    Py_ssize_t nargsf,
    int is_native) {
  PyTypeObject* type = (PyTypeObject*)PyTuple_GET_ITEM(info, 1);
  PyObject* name = PyTuple_GET_ITEM(info, 0);
  _PyType_VTable* vtable = (_PyType_VTable*)type->tp_cache;
  PyObject* mro = type->tp_mro;
  Py_ssize_t slot = PyLong_AsSsize_t(PyDict_GetItem(vtable->vt_slotmap, name));

  assert(vtable != NULL);
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i++) {
    PyObject* value = NULL;
    PyTypeObject* cur_type = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
    if (get_func_or_special_callable(cur_type, name, &value)) {
      return StaticError;
    }
    if (value != NULL) {
      PyObject* original = NULL;
      if (classloader_get_original_static_def(type, name, &original)) {
        Py_DECREF(value);
        return StaticError;
      }
      if (_PyVTable_setslot(type, name, slot, value, original)) {
        Py_XDECREF(original);
        Py_DECREF(value);
        return StaticError;
      }

      _PyClassLoader_StaticCallReturn res;
      if (is_native) {
        PyObject* callable = original;
        Py_ssize_t arg_count = _PyClassLoader_GetExpectedArgCount(&callable);
        if (arg_count < 0) {
          return StaticError;
        }

        PyObject* obj_res;
        if (PyFunction_Check(callable)) {
          PyCodeObject* code =
              (PyCodeObject*)((PyFunctionObject*)callable)->func_code;
          PyObject* call_args[arg_count];
          PyObject* free_args[arg_count];

          if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) < 0) {
            return StaticError;
          }

          obj_res =
              _PyClassLoader_InvokeMethod(vtable, slot, call_args, arg_count);
          _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
          if (obj_res != NULL) {
            int optional = 0, exact = 0, func_flags = 0, type_code;
            PyTypeObject* type = (PyTypeObject *)_PyClassLoader_ResolveReturnType(
                callable, &optional, &exact, &func_flags);
            if (type != NULL &&
                (type_code = _PyClassLoader_GetTypeCode(type)) !=
                    TYPED_OBJECT) {
              res.rax = (void*)_PyClassLoader_Unbox(obj_res, type_code);
            } else {
              res.rax = obj_res;
            }
          } else {
            res.rax = NULL;
          }
          res.rdx = (void*)(uint64_t)(obj_res != NULL);
        } else {
          assert(arg_count < 5);
          res.rax = _PyClassLoader_InvokeMethod(
              vtable, slot, (PyObject**)args, arg_count);
          res.rdx = (void*)(uint64_t)(res.rax != NULL);
        }

      } else {
        res.rax =
            _PyClassLoader_InvokeMethod(vtable, slot, (PyObject**)args, nargsf);
        res.rdx = (void*)(uint64_t)(res.rax != NULL);
      }

      Py_XDECREF(original);
      Py_DECREF(value);
      return res;
    }
  }

  PyErr_Format(
      CiExc_StaticTypeError, "'%s' has no attribute %U", type->tp_name, name);
  return StaticError;
}

__attribute__((__used__)) PyObject* _PyVTable_lazyinit_vectorcall(
    PyObject* thunk,
    PyObject** args,
    Py_ssize_t nargsf) {
  return (PyObject*)_PyVTable_lazyinit_impl(thunk, (void**)args, nargsf, 0)
      .rax;
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_lazyinit_native(PyObject* thunk, void** args) {
  return _PyVTable_lazyinit_impl(thunk, args, 0, 1);
}

VTABLE_THUNK(_PyVTable_lazyinit, PyObject)

/**
    For every slot in the vtable slotmap, this sets the vectorcall entrypoint
    to `_PyVTable_lazyinit`.
*/
int _PyClassLoader_ReinitVtable(PyTypeObject* type, _PyType_VTable* vtable) {
  PyObject *name, *slot;
  PyObject* slotmap = vtable->vt_slotmap;
  Py_ssize_t i = 0;
  while (PyDict_Next(slotmap, &i, &name, &slot)) {
    Py_ssize_t index = PyLong_AsSsize_t(slot);
    PyObject* tuple = PyTuple_New(2);
    if (tuple == NULL) {
      return -1;
    }

    PyTuple_SET_ITEM(tuple, 0, name);
    Py_INCREF(name);
    PyTuple_SET_ITEM(tuple, 1, (PyObject*)type);
    Py_INCREF(type);
    vtable->vt_entries[index].vte_state = tuple;
    vtable->vt_entries[index].vte_entry =
        (vectorcallfunc)_PyVTable_lazyinit_dont_bolt;
  }
  return 0;
}

int used_in_vtable_worker(PyObject* value) {
  // we'll emit invokes to untyped builtin methods
  if (Py_TYPE(value) == &PyMethodDescr_Type) {
    return 1;
  } else if (Py_TYPE(value) == &_PyType_CachedPropertyThunk) {
    return used_in_vtable_worker(_Py_CachedPropertyThunk_GetFunc(value));
  } else if (Py_TYPE(value) == &_PyType_AsyncCachedPropertyThunk) {
    return used_in_vtable_worker(_Py_AsyncCachedPropertyThunk_GetFunc(value));
  }
  if (Py_TYPE(value) == &_PyTypedDescriptorWithDefaultValue_Type) {
    return 1;
  }
  if (Py_TYPE(value) == &_PyType_TypedDescriptorThunk) {
    return 1;
  }
  return _PyClassLoader_IsStaticCallable(value);
}

int used_in_vtable(PyObject* value) {
  if (used_in_vtable_worker(value)) {
    return 1;
  } else if (
      Py_TYPE(value) == &PyStaticMethod_Type &&
      used_in_vtable_worker(Ci_PyStaticMethod_GetFunc(value))) {
    return 1;
  } else if (
      Py_TYPE(value) == &PyClassMethod_Type &&
      used_in_vtable_worker(Ci_PyClassMethod_GetFunc(value))) {
    return 1;
  } else if (Py_TYPE(value) == &PyProperty_Type) {
    PyObject* func = ((Ci_propertyobject*)value)->prop_get;
    if (func != NULL && used_in_vtable_worker(func)) {
      return 1;
    }
    func = ((Ci_propertyobject*)value)->prop_set;
    if (func != NULL && used_in_vtable_worker(func)) {
      return 1;
    }
  } else if (Py_TYPE(value) == &PyCachedPropertyWithDescr_Type) {
    PyObject* func = ((PyCachedPropertyDescrObject*)value)->func;
    if (used_in_vtable_worker(func)) {
      return 1;
    }
  } else if (Py_TYPE(value) == &PyAsyncCachedPropertyWithDescr_Type) {
    PyObject* func = ((PyAsyncCachedPropertyDescrObject*)value)->func;
    if (used_in_vtable_worker(func)) {
      return 1;
    }
  }

  return 0;
}

// Steals a reference to the `getter_tuple` and `setter_tuple` objects.
int update_property_slot(
    PyObject* slotmap,
    int* slot_index,
    PyObject* getter_tuple,
    PyObject* setter_tuple) {
  PyObject* getter_index = PyLong_FromLong((*slot_index)++);
  int err = PyDict_SetItem(slotmap, getter_tuple, getter_index);
  Py_DECREF(getter_index);
  Py_DECREF(getter_tuple);
  if (err) {
    Py_DECREF(setter_tuple);
    return -1;
  }
  PyObject* setter_index = PyLong_FromLong((*slot_index)++);
  err = PyDict_SetItem(slotmap, setter_tuple, setter_index);
  Py_DECREF(setter_index);
  Py_DECREF(setter_tuple);
  if (err) {
    return -1;
  }
  return 0;
}
/**
    Merges the slot map of our bases with our own members, initializing the
    map with the members which are defined in the current type but not the
    base type. Also, skips non-static callables that exist in tp_dict,
    because we cannot invoke against those anyway.
*/
int _PyClassLoader_UpdateSlotMap(PyTypeObject* self, PyObject* slotmap) {
  PyObject *key, *value;
  Py_ssize_t i;

  /* Add indexes for anything that is new in our class */
  int slot_index = PyDict_Size(slotmap);
  i = 0;
  while (PyDict_Next(_PyType_GetDict(self), &i, &key, &value)) {
    if (PyDict_GetItem(slotmap, key) || !used_in_vtable(value)) {
      /* we either share the same slot, or this isn't a static function,
       * so it doesn't need a slot */
      continue;
    }
    PyObject* index = PyLong_FromLong(slot_index++);
    int err = PyDict_SetItem(slotmap, key, index);
    Py_DECREF(index);
    if (err) {
      return -1;
    }
    PyTypeObject* val_type = Py_TYPE(value);
    if (val_type == &PyProperty_Type ||
        val_type == &PyCachedPropertyWithDescr_Type ||
        val_type == &PyAsyncCachedPropertyWithDescr_Type) {
      PyObject* getter_index = PyLong_FromLong(slot_index++);
      PyObject* getter_tuple = get_property_getter_descr_tuple(key);
      err = PyDict_SetItem(slotmap, getter_tuple, getter_index);
      Py_DECREF(getter_index);
      Py_DECREF(getter_tuple);
      if (err) {
        return -1;
      }
      PyObject* setter_index = PyLong_FromLong(slot_index++);
      PyObject* setter_tuple = get_property_setter_descr_tuple(key);
      err = PyDict_SetItem(slotmap, setter_tuple, setter_index);
      Py_DECREF(setter_index);
      Py_DECREF(setter_tuple);
      if (err) {
        return -1;
      }
    } else if (Py_TYPE(value) == &_PyTypedDescriptorWithDefaultValue_Type) {
      PyObject* getter_tuple = get_property_getter_descr_tuple(key);
      PyObject* setter_tuple = get_property_setter_descr_tuple(key);
      if (update_property_slot(
              slotmap, &slot_index, getter_tuple, setter_tuple) < 0) {
        return -1;
      }
    }
  }
  return 0;
}

int is_static_type(PyTypeObject* type) {
  return (type->tp_flags &
          (Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED |
           Ci_Py_TPFLAGS_GENERIC_TYPE_INST)) ||
      !(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
}

/**
    Creates a vtable for a type. Goes through the MRO, and recursively creates
   v-tables for any static base classes if needed.
*/
_PyType_VTable* _PyClassLoader_EnsureVtable(
    PyTypeObject* self,
    int init_subclasses) {
  _PyType_VTable* vtable = (_PyType_VTable*)self->tp_cache;
  PyObject* slotmap = NULL;
  PyObject* mro;

  if (self == &PyBaseObject_Type) {
    // We don't create a vtable for `object`. If we try to do that, all
    // subclasses of `object` (which is all classes), will need to have a
    // v-table of their own, and that's too much memory usage for almost no
    // benefit (since most classes are not Static). Also, none of the attributes
    // on `object` are interesting enough to invoke against.
    PyErr_SetString(
        PyExc_RuntimeError, "cannot initialize vtable for builtins.object");
    return NULL;
  }
  if (vtable != NULL) {
    return vtable;
  }

  mro = self->tp_mro;
  Py_ssize_t mro_size = PyTuple_GET_SIZE(mro);
  if (mro_size > 1) {
    /* TODO: Non-type objects in mro? */
    /* TODO: Multiple inheritance */

    /* Get the size of the next element which is a static class
     * in our mro, we'll build on it.  We don't care about any
     * non-static classes because we don't generate invokes to them */
    PyTypeObject* next;
    for (Py_ssize_t i = 1; i < mro_size; i++) {
      next = (PyTypeObject*)PyTuple_GET_ITEM(mro, i);
      if (is_static_type(next)) {
        break;
      }
    }

    assert(PyType_Check(next));
    assert(is_static_type(next));
    if (next != &PyBaseObject_Type) {
      _PyType_VTable* base_vtable = (_PyType_VTable*)next->tp_cache;
      if (base_vtable == NULL) {
        base_vtable = _PyClassLoader_EnsureVtable(next, 0);

        if (base_vtable == NULL) {
          return NULL;
        }

        if (init_subclasses && type_init_subclass_vtables(next)) {
          return NULL;
        }

        if (self->tp_cache != NULL) {
          /* we have recursively initialized the current v-table,
           * no need to continue with initialization now */
          return (_PyType_VTable*)self->tp_cache;
        }
      }

      PyObject* next_slotmap = base_vtable->vt_slotmap;
      assert(next_slotmap != NULL);

      slotmap = PyDict_Copy(next_slotmap);

      if (slotmap == NULL) {
        return NULL;
      }
    }
  }

  if (slotmap == NULL) {
    slotmap = _PyDict_NewPresized(PyDict_Size(_PyType_GetDict(self)));
  }

  if (slotmap == NULL) {
    return NULL;
  }

  if (is_static_type(self)) {
    if (_PyClassLoader_UpdateSlotMap(self, slotmap)) {
      Py_DECREF(slotmap);
      return NULL;
    }
  }

  /* finally allocate the vtable, which will have empty slots initially */
  Py_ssize_t slot_count = PyDict_Size(slotmap);
  vtable = PyObject_GC_NewVar(_PyType_VTable, &_PyType_VTableType, slot_count);

  if (vtable == NULL) {
    Py_DECREF(slotmap);
    return NULL;
  }
  vtable->vt_size = slot_count;
  vtable->vt_thunks = NULL;
  vtable->vt_original = NULL;
  vtable->vt_specials = NULL;
  vtable->vt_slotmap = slotmap;
  vtable->vt_typecode = TYPED_OBJECT;
  self->tp_cache = (PyObject*)vtable;
  memset(&vtable->vt_entries[0], 0, sizeof(_PyType_VTableEntry) * slot_count);

  if (_PyClassLoader_ReinitVtable(self, vtable)) {
    self->tp_cache = NULL;
    Py_DECREF(vtable);
    return NULL;
  }

  PyObject_GC_Track(vtable);

  if (init_subclasses && type_init_subclass_vtables(self)) {
    return NULL;
  }

  return vtable;
}

static int clear_vtables_recurse(PyTypeObject* type) {
  PyObject* subclasses = type->tp_subclasses;
  PyObject* ref;
  if (type->tp_cache != NULL) {
    // If the type has a type code we need to preserve it, but we'll clear
    // everything else
    int type_code = ((_PyType_VTable*)type->tp_cache)->vt_typecode;
    Py_CLEAR(type->tp_cache);
    if (type_code != TYPED_OBJECT) {
      _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(type, 0);
      if (vtable != NULL) {
        vtable->vt_typecode = type_code;
      }
    }
  }
  if (subclasses != NULL) {
    Py_ssize_t i = 0;
    while (PyDict_Next(subclasses, &i, NULL, &ref)) {
      assert(PyWeakref_CheckRef(ref));
      ref = PyWeakref_GET_OBJECT(ref);
      if (ref == Py_None) {
        continue;
      }

      assert(PyType_Check(ref));
      if (clear_vtables_recurse((PyTypeObject*)ref)) {
        return -1;
      }
    }
  }
  return 0;
}

int _PyClassLoader_ClearVtables() {
  /* Recursively clear all vtables.
   *
   * This is really only intended for use in tests to avoid state pollution.
   */
  return clear_vtables_recurse(&PyBaseObject_Type);
}

/*
    Fetches the member held at the path defined by a type descriptor.
    e.g: ("mymod", "MyClass", "my_member")

    When container is not NULL, populates it with the `PyTypeObject` of the
   container. When containerkey is not NULL, populates it with the member name.
   This could be a tuple in the case of properties, such as ("my_member",
   "fget").

    The lookup is done from `sys.modules` (tstate->interp->modules), and if a
   module is not found, this function will import it.
*/
static PyObject* classloader_get_member(
    PyObject* path,
    Py_ssize_t items,
    PyObject** container,
    PyObject** containerkey) {
  if (container) {
    *container = NULL;
  }
  if (containerkey) {
    *containerkey = NULL;
  }

  if (PyTuple_GET_SIZE(path) != 2) {
    PyErr_Format(
      CiExc_StaticTypeError,
      "bad descriptor: %R",
      path
    );
    return NULL;
  }

  PyObject *container_obj = _PyClassLoader_ResolveContainer(PyTuple_GET_ITEM(path, 0));
  if (container_obj == NULL) {
    return NULL;
  }

  PyObject *attr_name = PyTuple_GET_ITEM(path, 1);
  if (containerkey) {
    *containerkey = attr_name;
  }

  PyObject *attr;
  if (PyType_Check(container_obj)) {
    PyObject *type_dict = ((PyTypeObject*)container_obj)->tp_dict;
     if (!PyTuple_CheckExact(attr_name)) {
      attr = PyDict_GetItem(type_dict, attr_name);
      if (attr == NULL) {
        PyErr_Format(
            CiExc_StaticTypeError,
            "bad name provided for class loader, %R doesn't exist in type %s",
            attr_name,
            ((PyTypeObject *)container_obj)->tp_name);
          goto error;
      }
      Py_INCREF(attr);
    } else if (get_func_or_special_callable((PyTypeObject*)container_obj, attr_name, &attr) < 0) {
        goto error;
    }
  } else {
    attr = _PyClassLoader_GetModuleAttr(container_obj, attr_name);
  }

  if (attr == NULL) {
    goto error;
  }

  if (container) {
    *container = container_obj;
  } else {
    Py_DECREF(container_obj);
  }

  return attr;
error:
  Py_DECREF(container_obj);
  return NULL;
}

/**
    This function is called when a member on a previously unseen
    class is encountered.

    Given a type descriptor to a callable, this function:
    - Ensures that the containing class has a v-table.
    - Adds an entry to the global `classloader_cache`
      (so future slot index lookups are faster)
    - Initializes v-tables for all subclasses of the containing class
*/
static int classloader_init_slot(PyObject* path) {
  /* path is "mod.submod.Class.func", start search from
   * sys.modules */
  PyObject *classloader_cache = _PyClassLoader_GetCache();
  if (classloader_cache == NULL) {
    return -1;
  }

  PyObject* target_type = _PyClassLoader_ResolveContainer(PyTuple_GET_ITEM(path, 0));
  if (target_type == NULL) {
    return -1;
  } else if (_PyClassLoader_VerifyType(target_type, path)) {
    Py_XDECREF(target_type);
    return -1;
  }

  /* Now we need to update or make the v-table for this type */
  _PyType_VTable* vtable = _PyClassLoader_EnsureVtable((PyTypeObject *)target_type, 0);
  if (vtable == NULL) {
    Py_XDECREF(target_type);
    return -1;
  }

  PyObject* slot_map = vtable->vt_slotmap;
  PyObject* slot_name = PyTuple_GET_ITEM(path, PyTuple_GET_SIZE(path) - 1);
  PyObject* new_index = PyDict_GetItem(slot_map, slot_name);
  if (new_index == NULL) {
    PyErr_Format(
        PyExc_RuntimeError,
        "unable to resolve v-table slot %R in %s is_static: %s",
        slot_name,
        ((PyTypeObject *)target_type)->tp_name,
        is_static_type((PyTypeObject *)target_type) ? "true" : "false");
    Py_DECREF(target_type);
    return -1;
  }
  assert(new_index != NULL);

  if (PyDict_SetItem(classloader_cache, path, new_index) ||
      type_init_subclass_vtables((PyTypeObject *)target_type)) {
    Py_DECREF(target_type);
    return -1;
  }

  Py_DECREF(target_type);
  return 0;
}

/**
    Returns a slot index given a "path" (type descr tuple) to a method.
    e.g ("my_mod", "MyClass", "my_method")
*/
Py_ssize_t _PyClassLoader_ResolveMethod(PyObject* path) {
  PyObject *classloader_cache = _PyClassLoader_GetCache();
  if (classloader_cache == NULL) {
    return -1;
  }

  /* TODO: Should we gracefully handle when there are two
   * classes with the same name? */
  PyObject* slot_index_obj = PyDict_GetItem(classloader_cache, path);
  if (slot_index_obj == NULL) {
    if (classloader_init_slot(path)) {
      return -1;
    }
    slot_index_obj = PyDict_GetItem(classloader_cache, path);
  }
  return PyLong_AS_LONG(slot_index_obj);
}

PyObject* _PyClassLoader_ResolveFunction(PyObject* path, PyObject** container) {
  PyObject* containerkey;
  PyObject* func = classloader_get_member(
      path, PyTuple_GET_SIZE(path), container, &containerkey);

  PyObject* original = NULL;
  if (container != NULL && *container != NULL) {
    assert(containerkey != NULL);
    if (PyType_Check(*container)) {
      PyTypeObject* type = (PyTypeObject*)*container;
      if (type->tp_cache != NULL) {
        PyObject* originals = ((_PyType_VTable*)type->tp_cache)->vt_original;
        if (originals != NULL) {
          original = PyDict_GetItem(originals, containerkey);
        }
      }
    } else if (Ci_StrictModule_Check(*container)) {
      original = Ci_StrictModule_GetOriginal(*container, containerkey);
    }
  }
  if (original == func) {
    original = NULL;
  }

  if (original != NULL) {
    PyObject* res =
        (PyObject*)_PyClassLoader_GetOrMakeThunk(func, original, *container, containerkey);
    Py_DECREF(func);
    assert(res != NULL);
    return res;
  }

  if (func != NULL) {
    if (Py_TYPE(func) == &PyStaticMethod_Type) {
      PyObject* res = Ci_PyStaticMethod_GetFunc(func);
      Py_INCREF(res);
      Py_DECREF(func);
      func = res;
    } else if (Py_TYPE(func) == &PyClassMethod_Type) {
      PyObject* res = Ci_PyClassMethod_GetFunc(func);
      Py_INCREF(res);
      Py_DECREF(func);
      func = res;
    }
  }

  return func;
}

PyObject** _PyClassLoader_ResolveIndirectPtr(PyObject* path) {
  PyObject* container;
  PyObject* name;
  PyObject* func =
      classloader_get_member(path, PyTuple_GET_SIZE(path), &container, &name);
  if (func == NULL) {
    return NULL;
  }

  // for performance reason should only be used on mutable containers
  assert(!_PyClassLoader_IsImmutable(container));

  PyObject** cache = NULL;
  int use_thunk = 0;
  if (PyType_Check(container)) {
    _PyType_VTable* vtable =
        _PyClassLoader_EnsureVtable((PyTypeObject*)container, 1);
    if (vtable == NULL) {
      goto error;
    }
    use_thunk = 1;
  } else if (Ci_StrictModule_Check(container)) {
    use_thunk = 1;
  } else if (PyModule_Check(container)) {
    /* modules have no special translation on things we invoke, so
     * we just rely upon the normal JIT dict watchers */
    PyObject* dict = Ci_MaybeStrictModule_Dict(container);
    if (dict != NULL) {
      cache = _PyJIT_GetDictCache(dict, name);
    }
  }
  if (use_thunk) {
    /* we pass func in for original here.  Either the thunk will already exist
     * in which case the value has been patched, or it won't yet exist in which
     * case func is the original function in the type. */
    _Py_StaticThunk* thunk = _PyClassLoader_GetOrMakeThunk(func, func, container, name);
    if (thunk == NULL) {
      goto error;
    }

    cache = &thunk->thunk_funcref;
    Py_DECREF(thunk);
  }

error:
  Py_DECREF(container);
  Py_DECREF(func);
  return cache;
}

int _PyClassLoader_IsImmutable(PyObject* container) {
  if (PyType_Check(container)) {
    PyTypeObject* type = (PyTypeObject*)container;
#if PY_VERSION_HEX < 0x030C0000
    if (type->tp_flags & Ci_Py_TPFLAGS_FROZEN ||
        !(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
      return 1;
    }
#else
    if (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE ||
        !(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
      return 1;
    }
#endif
  }

  if (Ci_StrictModule_CheckExact(container) &&
      ((Ci_StrictModuleObject*)container)->global_setter == NULL) {
    return 1;
  }
  return 0;
}

PyMethodDescrObject* _PyClassLoader_ResolveMethodDef(PyObject* path) {
  PyTypeObject* target_type;
  PyObject* cur = classloader_get_member(
      path, PyTuple_GET_SIZE(path), (PyObject**)&target_type, NULL);

  if (cur == NULL) {
    assert(target_type == NULL);
    return NULL;
  } else if (
      _PyClassLoader_VerifyType((PyObject*)target_type, path) ||
      target_type->tp_flags & Py_TPFLAGS_BASETYPE) {
    Py_XDECREF(target_type);
    Py_DECREF(cur);
    return NULL;
  }

  Py_DECREF(target_type);
  if (Py_TYPE(cur) == &PyMethodDescr_Type) {
    return (PyMethodDescrObject*)cur;
  }

  Py_DECREF(cur);
  return NULL;
}

int _PyClassLoader_AddSubclass(PyTypeObject* base, PyTypeObject* type) {
  if (base->tp_cache == NULL) {
    /* nop if base class vtable isn't initialized */
    return 0;
  }

  _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(type, 0);
  if (vtable == NULL) {
    return -1;
  }
  return 0;
}

int _PyClassLoader_NotifyDictChange(PyDictObject* dict, PyObject* key) {
  return _PyClassLoader_CheckModuleChange(dict, key);
}

static int classloader_init_field(PyObject* path, int* field_type) {
  /* path is "mod.submod.Class.func", start search from
   * sys.modules */
  PyObject* cur =
      classloader_get_member(path, PyTuple_GET_SIZE(path), NULL, NULL);
  if (cur == NULL) {
    return -1;
  }

  if (Py_TYPE(cur) == &PyMemberDescr_Type) {
    if (field_type != NULL) {
      switch (((PyMemberDescrObject*)cur)->d_member->type) {
        case T_BYTE:
          *field_type = TYPED_INT8;
          break;
        case T_SHORT:
          *field_type = TYPED_INT16;
          break;
        case T_INT:
          *field_type = TYPED_INT32;
          break;
        case T_LONG:
          *field_type = TYPED_INT64;
          break;
        case T_UBYTE:
          *field_type = TYPED_UINT8;
          break;
        case T_USHORT:
          *field_type = TYPED_UINT16;
          break;
        case T_UINT:
          *field_type = TYPED_UINT32;
          break;
        case T_ULONG:
          *field_type = TYPED_UINT64;
          break;
        case T_BOOL:
          *field_type = TYPED_BOOL;
          break;
        case T_DOUBLE:
          *field_type = TYPED_DOUBLE;
          break;
        case T_FLOAT:
          *field_type = TYPED_SINGLE;
          break;
        case T_CHAR:
          *field_type = TYPED_CHAR;
          break;
        case T_OBJECT_EX:
          *field_type = TYPED_OBJECT;
          break;
        default:
          Py_DECREF(cur);
          PyErr_Format(PyExc_ValueError, "unknown static type: %S", path);
          return -1;
      }
    }
    Py_DECREF(cur);
    Py_ssize_t offset = ((PyMemberDescrObject*)cur)->d_member->offset;
    return offset;
  } else if (Py_TYPE(cur) == &_PyTypedDescriptor_Type) {
    if (field_type != NULL) {
      *field_type = TYPED_OBJECT;
      assert(((_PyTypedDescriptor*)cur)->td_offset % sizeof(Py_ssize_t) == 0);
    }
    Py_DECREF(cur);
    return ((_PyTypedDescriptor*)cur)->td_offset;
  } else if (Py_TYPE(cur) == &_PyTypedDescriptorWithDefaultValue_Type) {
    if (field_type != NULL) {
      *field_type = TYPED_OBJECT;
      assert(
          ((_PyTypedDescriptorWithDefaultValue*)cur)->td_offset %
              sizeof(Py_ssize_t) ==
          0);
    }
    Py_DECREF(cur);
    return ((_PyTypedDescriptorWithDefaultValue*)cur)->td_offset;
  }

  Py_DECREF(cur);
  PyErr_Format(CiExc_StaticTypeError, "bad field for class loader %R", path);
  return -1;
}

/* Resolves the offset for a given field, returning -1 on failure with an error
 * set or the field offset.  Path is a tuple in the form
 * ('module', 'class', 'field_name')
 */
Py_ssize_t _PyClassLoader_ResolveFieldOffset(PyObject* path, int* field_type) {
  PyObject *classloader_cache = _PyClassLoader_GetCache();
  if (classloader_cache == NULL) {
  }

  /* TODO: Should we gracefully handle when there are two
   * classes with the same name? */
  PyObject* slot_index_obj = PyDict_GetItem(classloader_cache, path);
  if (slot_index_obj != NULL) {
    PyObject* offset = PyTuple_GET_ITEM(slot_index_obj, 0);
    if (field_type != NULL) {
      PyObject* type = PyTuple_GET_ITEM(slot_index_obj, 1);
      *field_type = PyLong_AS_LONG(type);
    }
    return PyLong_AS_LONG(offset);
  }

  int tmp_field_type = 0;
  Py_ssize_t slot_index = classloader_init_field(path, &tmp_field_type);
  if (slot_index < 0) {
    return -1;
  }
  slot_index_obj = PyLong_FromLong(slot_index);
  if (slot_index_obj == NULL) {
    return -1;
  }

  PyObject* field_type_obj = PyLong_FromLong(tmp_field_type);
  if (field_type_obj == NULL) {
    Py_DECREF(slot_index);
    return -1;
  }

  PyObject* cache = PyTuple_New(2);
  if (cache == NULL) {
    Py_DECREF(slot_index_obj);
    Py_DECREF(field_type_obj);
    return -1;
  }
  PyTuple_SET_ITEM(cache, 0, slot_index_obj);
  PyTuple_SET_ITEM(cache, 1, field_type_obj);

  if (PyDict_SetItem(classloader_cache, path, cache)) {
    Py_DECREF(cache);
    return -1;
  }

  Py_DECREF(cache);
  if (field_type != NULL) {
    *field_type = tmp_field_type;
  }

  return slot_index;
}

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfoFromThunk(
    PyObject* thunk,
    PyObject* container,
    int only_primitives) {
  if (!_PyClassLoader_IsPatchedThunk(thunk)) {
    return NULL;
  }
  PyObject* originals = NULL;
  if (PyType_Check(container)) {
    PyObject* vtable = ((PyTypeObject*)container)->tp_cache;
    originals = ((_PyType_VTable*)vtable)->vt_original;
  } else if (Ci_StrictModule_Check(container)) {
    originals = ((Ci_StrictModuleObject*)container)->originals;
  }
  if (!originals) {
    return NULL;
  }
  PyObject* original = PyDict_GetItem(
      originals, ((_Py_StaticThunk*)thunk)->thunk_tcs.tcs_rt.rt_name);
  if (original == NULL) {
    return NULL;
  }
  PyObject* unwrapped = _PyClassLoader_MaybeUnwrapCallable(original);
  if (unwrapped != NULL) {
    original = unwrapped;
  }
  PyObject* code = PyFunction_GetCode(original);
  if (code == NULL) {
    return NULL;
  }
  return _PyClassLoader_GetTypedArgsInfo((PyCodeObject*)code, only_primitives);
}

int _PyClassLoader_HasPrimitiveArgs(PyCodeObject* code) {
  PyObject* checks = _PyClassLoader_GetCodeArgumentTypeDescrs(code);
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    PyObject* type_descr = PyTuple_GET_ITEM(checks, i + 1);

    if (_PyClassLoader_ResolvePrimitiveType(type_descr) != TYPED_OBJECT) {
      return 1;
    }
  }
  return 0;
}

static PyObject* invoke_native_helper = NULL;

static inline int import_invoke_native() {
  if (__builtin_expect(invoke_native_helper == NULL, 0)) {
    PyObject* native_utils = PyImport_ImportModule("__static__.native_utils");
    if (native_utils == NULL) {
      return -1;
    }
    invoke_native_helper =
        PyObject_GetAttrString(native_utils, "invoke_native");
    Py_DECREF(native_utils);
    if (invoke_native_helper == NULL) {
      return -1;
    }
  }
  return 0;
}

PyObject* _PyClassloader_InvokeNativeFunction(
    PyObject* lib_name,
    PyObject* symbol_name,
    PyObject* signature,
    PyObject** args,
    Py_ssize_t nargs) {
  if (!PyUnicode_CheckExact(lib_name)) {
    PyErr_Format(
        PyExc_RuntimeError,
        "'lib_name' must be a str, got '%s'",
        Py_TYPE(lib_name)->tp_name);
    return NULL;
  }
  if (!PyUnicode_CheckExact(lib_name)) {
    PyErr_Format(
        PyExc_RuntimeError,
        "'symbol_name' must be a str, got '%s'",
        Py_TYPE(lib_name)->tp_name);
    return NULL;
  }
  if (!PyTuple_CheckExact(signature)) {
    PyErr_Format(
        PyExc_RuntimeError,
        "'signature' must be a tuple of type descriptors",
        Py_TYPE(lib_name)->tp_name);
    return NULL;
  }

  int return_typecode =
      _PyClassLoader_ResolvePrimitiveType(PyTuple_GET_ITEM(signature, nargs));
  if (return_typecode == -1) {
    // exception must be set already
    assert(PyErr_Occurred());
    return NULL;
  }

  // build arg tuple.. this is kinda wasteful, but we're not optimizing for the
  // interpreter here
  PyObject* arguments = PyTuple_New(nargs);
  if (arguments == NULL) {
    return NULL;
  }
  for (int i = 0; i < nargs; i++) {
    PyTuple_SET_ITEM(arguments, i, args[i]);
    Py_INCREF(args[i]);
  }

  if (import_invoke_native() < 0) {
    return NULL;
  }
  PyObject* res = PyObject_CallFunction(
      invoke_native_helper,
      "OOOO",
      lib_name,
      symbol_name,
      signature,
      arguments);

  Py_DECREF(arguments);
  return res;
}

// Returns the size of the dlsym_cache dict (0 if uninitialized)
PyObject* _PyClassloader_SizeOf_DlSym_Cache() {
  if (dlsym_cache == NULL) {
    return PyLong_FromLong(0);
  }
  Py_ssize_t size = PyDict_Size(dlsym_cache);
  return PyLong_FromSsize_t(size);
}

// Returns the size of the dlopen_cache dict (0 if uninitialized)
PyObject* _PyClassloader_SizeOf_DlOpen_Cache() {
  if (dlopen_cache == NULL) {
    return PyLong_FromLong(0);
  }
  Py_ssize_t size = PyDict_Size(dlopen_cache);
  return PyLong_FromSsize_t(size);
}

// Clears the dlsym_cache dict
void _PyClassloader_Clear_DlSym_Cache() {
  if (dlsym_cache != NULL) {
    PyDict_Clear(dlsym_cache);
  }
}

// Clears the dlopen_cache dict
void _PyClassloader_Clear_DlOpen_Cache() {
  if (dlopen_cache != NULL) {
    PyObject *name, *handle;
    Py_ssize_t i = 0;
    while (PyDict_Next(dlopen_cache, &i, &name, &handle)) {
      void* raw_handle = PyLong_AsVoidPtr(handle);
      // Ignore errors - we can't do much even if they occur
      dlclose(raw_handle);
    }

    PyDict_Clear(dlopen_cache);
  }
}

// Thin wrapper over dlopen, returns the handle of the opened lib
static void* classloader_dlopen(PyObject* lib_name) {
  assert(PyUnicode_CheckExact(lib_name));
  const char* raw_lib_name = PyUnicode_AsUTF8(lib_name);
  if (raw_lib_name == NULL) {
    return NULL;
  }
  void* handle = dlopen(raw_lib_name, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    PyErr_Format(
        PyExc_RuntimeError,
        "classloader: Could not load library '%s': %s",
        raw_lib_name,
        dlerror());
    return NULL;
  }
  return handle;
}

// Looks up the cached handle to the shared lib of given name. If not found,
// proceeds to load it and populates the cache.
static void* classloader_lookup_sharedlib(PyObject* lib_name) {
  assert(PyUnicode_CheckExact(lib_name));
  PyObject* val = NULL;

  // Ensure cache exists
  if (dlopen_cache == NULL) {
    dlopen_cache = PyDict_New();
    if (dlopen_cache == NULL) {
      return NULL;
    }
  }

  val = PyDict_GetItem(dlopen_cache, lib_name);
  if (val != NULL) {
    // Cache hit
    return PyLong_AsVoidPtr(val);
  }

  // Lookup the lib
  void* handle = classloader_dlopen(lib_name);
  if (handle == NULL) {
    return NULL;
  }

  // Populate the cache with the handle
  val = PyLong_FromVoidPtr(handle);
  if (val == NULL) {
    return NULL;
  }
  int res = PyDict_SetItem(dlopen_cache, lib_name, val);
  Py_DECREF(val);
  if (res < 0) {
    return NULL;
  }
  return handle;
}

// Wrapper over `dlsym`.
static PyObject* classloader_lookup_symbol(
    PyObject* lib_name,
    PyObject* symbol_name) {
  void* handle = classloader_lookup_sharedlib(lib_name);
  if (handle == NULL) {
    assert(PyErr_Occurred());
    return NULL;
  }

  const char* raw_symbol_name = PyUnicode_AsUTF8(symbol_name);
  if (raw_symbol_name == NULL) {
    return NULL;
  }

  void* res = dlsym(handle, raw_symbol_name);
  if (res == NULL) {
    // Technically, `res` could actually have the value `NULL`, but we're
    // in the business of looking up callables, so we raise an exception
    // (NULL cannot be called anyway).
    //
    // To be 100% correct, we could clear existing errors with `dlerror`,
    // call `dlsym` and then call `dlerror` again, to check whether an
    // error occured, but that'll be more work than we need.
    PyErr_Format(
        PyExc_RuntimeError,
        "classloader: unable to lookup '%U' in '%U': %s",
        symbol_name,
        lib_name,
        dlerror());
    return NULL;
  }

  PyObject* symbol = PyLong_FromVoidPtr(res);
  if (symbol == NULL) {
    return NULL;
  }
  return symbol;
}

// Looks up the raw symbol address from the given lib, and returns
// a boxed value of it.
void* _PyClassloader_LookupSymbol(PyObject* lib_name, PyObject* symbol_name) {
  if (!PyUnicode_CheckExact(lib_name)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "classloader: 'lib_name' must be a str, got '%s'",
        Py_TYPE(lib_name)->tp_name);
    return NULL;
  }
  if (!PyUnicode_CheckExact(symbol_name)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "classloader: 'symbol_name' must be a str, got '%s'",
        Py_TYPE(symbol_name)->tp_name);
    return NULL;
  }

  // Ensure cache exists
  if (dlsym_cache == NULL) {
    dlsym_cache = PyDict_New();
    if (dlsym_cache == NULL) {
      return NULL;
    }
  }

  PyObject* key = PyTuple_Pack(2, lib_name, symbol_name);
  if (key == NULL) {
    return NULL;
  }

  PyObject* res = PyDict_GetItem(dlsym_cache, key);

  if (res != NULL) {
    Py_DECREF(key);
    return PyLong_AsVoidPtr(res);
  }

  res = classloader_lookup_symbol(lib_name, symbol_name);
  if (res == NULL) {
    Py_DECREF(key);
    return NULL;
  }

  if (PyDict_SetItem(dlsym_cache, key, res) < 0) {
    Py_DECREF(key);
    Py_DECREF(res);
    return NULL;
  }

  void* addr = PyLong_AsVoidPtr(res);
  Py_DECREF(key);
  Py_DECREF(res);
  return addr;
}
