/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/classloader.h"

#include "internal/pycore_pystate.h" // @donotremove

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h" // @donotremove
#include "cinderx/Common/func.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/modulethunks.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
#include "cinderx/module_c_state.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

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

PyObject* classloader_get_property_method(
    PyTypeObject* type,
    PyObject* property,
    PyTupleObject* name);

int _PyClassLoader_IsPatchedThunk(PyObject* obj) {
  if (obj != NULL && Py_TYPE(obj) == &_PyType_StaticThunk) {
    return 1;
  }
  return 0;
}

static int clear_vtables_recurse(PyTypeObject* type) {
  PyObject* subclasses = type->tp_subclasses;
#if PY_VERSION_HEX >= 0x030C0000
  if (type->tp_flags & _Py_TPFLAGS_STATIC_BUILTIN) {
    PyInterpreterState* interp = _PyInterpreterState_GET();
    managed_static_type_state* state = Cix_PyStaticType_GetState(interp, type);
    subclasses = state->tp_subclasses;
  }
#endif

  PyObject* ref;
  if (type->tp_cache != NULL) {
    _PyType_VTable* old_vtable = (_PyType_VTable*)type->tp_cache;
    type->tp_cache = NULL;

    if (old_vtable->vt_typecode != TYPED_OBJECT || old_vtable->vt_gtr != NULL) {
      // If the type has a type code or references to its generic parameters
      // we need to preserve them, but we'll keep everything else
      _PyType_VTable* vtable = _PyClassLoader_EnsureVtable(type, 0);
      if (vtable == NULL) {
        type->tp_cache = (PyObject*)old_vtable;
        // We were unable to create the new vtable, and so we don't
        // want to lose track of the type code or the generic type
        // tracking.
        return -1;
      }

      vtable->vt_typecode = old_vtable->vt_typecode;
      vtable->vt_gtr = old_vtable->vt_gtr;
    }

    old_vtable->vt_gtr = NULL; // don't free it, we'll move it to the new vtable
    Py_CLEAR(old_vtable);
  }
  if (subclasses != NULL) {
    Py_ssize_t i = 0;
    while (PyDict_Next(subclasses, &i, NULL, &ref)) {
      assert(PyWeakref_CheckRef(ref));
      int err = PyWeakref_GetRef(ref, &ref);
      if (err < 0) {
        return -1;
      } else if (!err) {
        continue;
      }

      assert(PyType_Check(ref));
      if (clear_vtables_recurse((PyTypeObject*)ref)) {
        Py_DECREF(ref);
        return -1;
      }
      Py_DECREF(ref);
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
  PyObject* classloader_cache = _PyClassLoader_GetCache();
  if (classloader_cache == NULL) {
    return -1;
  }

  PyObject* target_type =
      _PyClassLoader_ResolveContainer(PyTuple_GET_ITEM(path, 0));
  if (target_type == NULL) {
    return -1;
  } else if (_PyClassLoader_VerifyType(target_type, path)) {
    Py_XDECREF(target_type);
    return -1;
  }

  /* Now we need to update or make the v-table for this type */
  _PyType_VTable* vtable =
      _PyClassLoader_EnsureVtable((PyTypeObject*)target_type, 0);
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
        ((PyTypeObject*)target_type)->tp_name,
        is_static_type((PyTypeObject*)target_type) ? "true" : "false");
    Py_DECREF(target_type);
    return -1;
  }
  assert(new_index != NULL);

  if (PyDict_SetItem(classloader_cache, path, new_index) ||
      _PyClassLoader_InitSubclassVtables((PyTypeObject*)target_type)) {
    Py_DECREF(target_type);
    return -1;
  }

  Py_DECREF(target_type);
  return 0;
}

static PyObject* get_original(PyObject* container, PyObject* containerkey) {
  PyObject* original = NULL;
  if (PyType_Check(container)) {
    PyTypeObject* type = (PyTypeObject*)container;
    if (type->tp_cache != NULL) {
      PyObject* originals = ((_PyType_VTable*)type->tp_cache)->vt_original;
      if (originals != NULL) {
        original = PyDict_GetItem(originals, containerkey);
      }
    }
  } else if (Ci_StrictModule_Check(container)) {
    original = Ci_StrictModule_GetOriginal(container, containerkey);
  }
  return original;
}

/**
    Returns a slot index given a "path" (type descr tuple) to a method.
    e.g ("my_mod", "MyClass", "my_method")
*/
Py_ssize_t _PyClassLoader_ResolveMethod(PyObject* path) {
  PyObject* classloader_cache = _PyClassLoader_GetCache();
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
  PyObject* func = _PyClassLoader_ResolveMember(
      path, PyTuple_GET_SIZE(path), container, &containerkey);

  PyObject* original = NULL;
  if (container != NULL && *container != NULL) {
    assert(containerkey != NULL);
    original = get_original(*container, containerkey);
  }
  if (original == func) {
    original = NULL;
  }

  if (original != NULL) {
    PyObject* res = (PyObject*)_PyClassLoader_GetOrMakeThunk(
        func, original, *container, containerkey);
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
  } else if (PyTuple_GET_SIZE(path) == 2) {
    // Check if the value has been deleted from the type and if so
    // provide a better error message.
    PyObject* container_obj =
        _PyClassLoader_ResolveContainer(PyTuple_GET_ITEM(path, 0));
    if (container_obj == NULL) {
      PyErr_Format(CiExc_StaticTypeError, "Unknown type or module: %R", path);
      return NULL;
    }

    PyObject* attr_name = PyTuple_GET_ITEM(path, 1);
    original = get_original(container_obj, attr_name);
    if (original != NULL) {
      if (PyType_Check(container_obj)) {
        PyErr_Format(
            CiExc_StaticTypeError,
            "%s.%U has been deleted from class",
            ((PyTypeObject*)container_obj)->tp_name,
            attr_name);
      } else {
        PyErr_Clear();
        PyObject* dir = PyObject_Dir(container_obj);
        if (dir != NULL) {
          PyObject* comma = PyUnicode_FromString(", ");
          if (comma != NULL) {
            PyObject* dir_str = PyUnicode_Join(comma, dir);
            Py_DECREF(comma);
            Py_DECREF(dir);
            dir = dir_str;
          } else {
            Py_CLEAR(dir);
          }
        }
        if (dir == NULL) {
          PyErr_Clear();
          dir = Py_None;
        }
        PyErr_Format(
            CiExc_StaticTypeError,
            "%R.%U (%s) has been deleted from container, original was %R, "
            "contents are %R",
            container_obj,
            attr_name,
            Py_TYPE(container_obj)->tp_name,
            original,
            dir);
        Py_DECREF(dir);
      }
    }
    Py_DECREF(container_obj);
  }

  return func;
}

PyObject** _PyClassLoader_ResolveIndirectPtr(PyObject* path) {
  PyObject* container;
  PyObject* name;
  PyObject* func = _PyClassLoader_ResolveMember(
      path, PyTuple_GET_SIZE(path), &container, &name);
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
    if (!hasOnlyUnicodeKeys(PyModule_GetDict(container))) {
      goto error;
    }
    /* modules have no special translation on things we invoke, so
     * we just rely upon the normal JIT dict watchers */
    PyObject* dict = Ci_MaybeStrictModule_Dict(container);
    if (dict != NULL) {
      cache = Ci_GetDictCache(dict, name);
    }
  }
  if (use_thunk) {
    /* we pass func in for original here.  Either the thunk will already exist
     * in which case the value has been patched, or it won't yet exist in which
     * case func is the original function in the type. */
    _Py_StaticThunk* thunk =
        _PyClassLoader_GetOrMakeThunk(func, func, container, name);
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

PyMethodDescrObject* _PyClassLoader_ResolveMethodDef(PyObject* path) {
  PyTypeObject* target_type;
  PyObject* cur = _PyClassLoader_ResolveMember(
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

int _PyClassLoader_NotifyDictChange(
    PyDictObject* dict,
    PyDict_WatchEvent event,
    PyObject* key,
    PyObject* value) {
  if (_PyClassLoader_CheckModuleChange(dict, key) < 0 ||
      _PyClassLoader_CheckSubclassChange(dict, event, key, value) < 0) {
    return -1;
  }

  return 0;
}

static int classloader_init_field(PyObject* path, int* field_type) {
  /* path is "mod.submod.Class.func", start search from
   * sys.modules */
  PyObject* cur =
      _PyClassLoader_ResolveMember(path, PyTuple_GET_SIZE(path), NULL, NULL);
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
  PyObject* classloader_cache = _PyClassLoader_GetCache();
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

// Python list used to cache values
static PyObject* value_cache;
// Dictionary of cached object -> index
static PyObject* value_indices;
// Current offset for type cache. When the type cache is cleared this
// gets incremented by the current size so that we don't reuse previous
// slots and any existing caches will fail.
static int32_t type_index_offset;

void _PyClassLoader_ClearValueCache() {
  if (value_cache == NULL) {
    return;
  }
  type_index_offset += PyList_Size(value_cache);
  Py_CLEAR(value_cache);
  Py_CLEAR(value_indices);
}

int32_t _PyClassLoader_CacheValue(PyObject* value) {
  if (value_cache == NULL) {
    value_cache = PyList_New(0);
    if (value_cache == NULL) {
      return -1;
    }
  }
  if (value_indices == NULL) {
    value_indices = PyDict_New();
    if (value_indices == NULL) {
      return -1;
    }
  }
  PyObject* index = PyDict_GetItem(value_indices, (PyObject*)value);
  if (index != NULL) {
    return PyLong_AsLong(index);
  }

  Py_ssize_t iindex = PyList_GET_SIZE(value_cache) + type_index_offset;
  if (iindex >= INT32_MAX) {
    return -1;
  }

  PyObject* pyindex = PyLong_FromLong(iindex);
  if (pyindex == NULL) {
    return -1;
  }

  if (PyList_Append(value_cache, value) < 0) {
    Py_DECREF(pyindex);
    return -1;
  }

  if (PyDict_SetItem(value_indices, value, pyindex) < 0) {
    Py_SET_SIZE((PyVarObject*)value_cache, iindex - type_index_offset);
    Py_DECREF(pyindex);
    return -1;
  }
  Py_DECREF(pyindex);
  return (int32_t)iindex;
}

PyObject* _PyClassLoader_GetCachedValue(int32_t type) {
  if (value_cache == NULL || type < type_index_offset) {
    return NULL;
  }
  type -= type_index_offset;
  return Py_XNewRef(PyList_GetItem(value_cache, type));
}
