// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/type.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/generic_type.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

static PyObject* classloader_cache;
static PyObject* classloader_cache_module_to_keys;

Py_ssize_t _PyClassLoader_PrimitiveTypeToSize(int primitive_type) {
  switch (primitive_type) {
    case TYPED_INT8:
      return sizeof(char);
    case TYPED_INT16:
      return sizeof(short);
    case TYPED_INT32:
      return sizeof(int);
    case TYPED_INT64:
      return sizeof(long);
    case TYPED_UINT8:
      return sizeof(unsigned char);
    case TYPED_UINT16:
      return sizeof(unsigned short);
    case TYPED_UINT32:
      return sizeof(unsigned int);
    case TYPED_UINT64:
      return sizeof(unsigned long);
    case TYPED_BOOL:
      return sizeof(char);
    case TYPED_DOUBLE:
      return sizeof(double);
    case TYPED_SINGLE:
      return sizeof(float);
    case TYPED_CHAR:
      return sizeof(char);
    case TYPED_OBJECT:
      return sizeof(PyObject*);
    default:
      PyErr_Format(PyExc_ValueError, "unknown struct type: %d", primitive_type);
      return -1;
  }
}

int _PyClassLoader_PrimitiveTypeToStructMemberType(int primitive_type) {
  switch (primitive_type) {
    case TYPED_INT8:
      return T_BYTE;
    case TYPED_INT16:
      return T_SHORT;
    case TYPED_INT32:
      return T_INT;
    case TYPED_INT64:
      return T_LONG;
    case TYPED_UINT8:
      return T_UBYTE;
    case TYPED_UINT16:
      return T_USHORT;
    case TYPED_UINT32:
      return T_UINT;
    case TYPED_UINT64:
      return T_ULONG;
    case TYPED_BOOL:
      return T_BOOL;
    case TYPED_DOUBLE:
      return T_DOUBLE;
    case TYPED_SINGLE:
      return T_FLOAT;
    case TYPED_CHAR:
      return T_CHAR;
    case TYPED_OBJECT:
      return T_OBJECT_EX;
    default:
      PyErr_Format(PyExc_ValueError, "unknown struct type: %d", primitive_type);
      return -1;
  }
}

PyObject* _PyClassLoader_Box(uint64_t value, int primitive_type) {
  PyObject* new_val;
  double dbl;
  switch (primitive_type) {
    case TYPED_BOOL:
      new_val = value ? Py_True : Py_False;
      Py_INCREF(new_val);
      break;
    case TYPED_INT8:
      new_val = PyLong_FromLong((int8_t)value);
      break;
    case TYPED_INT16:
      new_val = PyLong_FromLong((int16_t)value);
      break;
    case TYPED_INT32:
      new_val = PyLong_FromLong((int32_t)value);
      break;
    case TYPED_INT64:
      new_val = PyLong_FromSsize_t((Py_ssize_t)value);
      break;
    case TYPED_UINT8:
      new_val = PyLong_FromUnsignedLong((uint8_t)value);
      break;
    case TYPED_UINT16:
      new_val = PyLong_FromUnsignedLong((uint16_t)value);
      break;
    case TYPED_UINT32:
      new_val = PyLong_FromUnsignedLong((uint32_t)value);
      break;
    case TYPED_UINT64:
      new_val = PyLong_FromSize_t((size_t)value);
      break;
    case TYPED_DOUBLE:
      memcpy(&dbl, &value, sizeof(double));
      new_val = PyFloat_FromDouble(dbl);
      break;
    default:
      assert(0);
      PyErr_SetString(PyExc_RuntimeError, "unsupported primitive type");
      new_val = NULL;
      break;
  }
  return new_val;
}

uint64_t _PyClassLoader_Unbox(PyObject* value, int primitive_type) {
  uint64_t new_val;
  double res;
  switch (primitive_type) {
    case TYPED_BOOL:
      new_val = value == Py_True ? 1 : 0;
      break;
    case TYPED_INT8:
    case TYPED_INT16:
    case TYPED_INT32:
    case TYPED_INT64:
      new_val = (uint64_t)PyLong_AsLong(value);
      break;
    case TYPED_UINT8:
    case TYPED_UINT16:
    case TYPED_UINT32:
    case TYPED_UINT64:
      new_val = (uint64_t)PyLong_AsUnsignedLong(value);
      break;
    case TYPED_DOUBLE:
      res = PyFloat_AsDouble(value);
      memcpy(&new_val, &res, sizeof(double));
      break;
    default:
      assert(0);
      PyErr_SetString(PyExc_RuntimeError, "unsupported primitive type");
      new_val = 0;
      break;
  }
  return new_val;
}

static PyObject*
classloader_instantiate_generic(PyObject* gtd, PyObject* name, PyObject* path) {
  if (!PyType_Check(gtd)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "generic type instantiation without type: %R on "
        "%R from %s",
        path,
        name,
        gtd->ob_type->tp_name);
    return NULL;
  }
  PyObject* tmp_tuple = PyTuple_New(PyTuple_GET_SIZE(name));
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(name); i++) {
    int optional, exact;
    PyObject* param = (PyObject*)_PyClassLoader_ResolveType(
        PyTuple_GET_ITEM(name, i), &optional, &exact);
    if (param == NULL) {
      Py_DECREF(tmp_tuple);
      return NULL;
    }
    if (optional) {
      PyObject* union_obj = Cix_Py_union_type_or(param, Py_None);
      if (union_obj == NULL) {
        Py_DECREF(tmp_tuple);
        return NULL;
      }
      param = union_obj;
    }
    PyTuple_SET_ITEM(tmp_tuple, i, param);
  }

  PyObject* next = _PyClassLoader_GetGenericInst(
      gtd, ((PyTupleObject*)tmp_tuple)->ob_item, PyTuple_GET_SIZE(tmp_tuple));
  Py_DECREF(tmp_tuple);
  return next;
}

PyObject* _PyClassLoader_GetModuleAttr(PyObject* module, PyObject* name) {
  if (!PyModule_CheckExact(module)) {
    return PyObject_GetAttr(module, name);
  }
  PyObject* module_dict = PyModule_GetDict(module);
  PyObject* res = PyDict_GetItem(module_dict, name);
  if (res == NULL) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "bad name provided for class loader, %R has no member %R",
        module,
        name);
    return NULL;
  }
  Py_INCREF(res);
  return res;
}

static PyObject* resolve_module(PyThreadState* tstate, PyObject* module_name) {
  PyObject* sys_modules = CI_INTERP_IMPORT_FIELD(tstate->interp, modules);

  if (sys_modules == NULL) {
    PyErr_Format(
        PyExc_RuntimeError,
        "classloader_get_member() when import system is pre-init or "
        "post-teardown");
    return NULL;
  }

  PyObject* module = PyDict_GetItem(sys_modules, module_name);
  if (module == NULL) {
    PyObject* mod =
        PyImport_ImportModuleLevelObject(module_name, NULL, NULL, NULL, 0);
    if (mod == NULL) {
      PyObject *et = NULL, *ev = NULL, *tb = NULL;
      PyErr_Fetch(&et, &ev, &tb);
      PyErr_Format(
          CiExc_StaticTypeError, "Could not load module %R", module_name);
#if PY_VERSION_HEX >= 0x030C0000
      _PyErr_ChainExceptions1(ev);
#else
      _PyErr_ChainExceptions(et, ev, tb);
#endif
      return NULL;
    }
    module = PyDict_GetItem(sys_modules, module_name);
    assert(module != NULL);
  }

  return module;
}

// Resolves a container (module or type) to the underlying object.
// Descriptor is in the format (module_name, type_name | None)
PyObject* _PyClassLoader_ResolveContainer(PyObject* container_path) {
  if (!PyTuple_Check(container_path)) {
    PyErr_Format(
        PyExc_TypeError,
        "bad type descriptor, expected module and type %R",
        container_path);
    return NULL;
  }

  PyObject* module_name = PyTuple_GET_ITEM(container_path, 0);
  PyThreadState* tstate = PyThreadState_GET();
  PyObject* module = resolve_module(tstate, module_name);
  if (module == NULL) {
    return NULL;
  }

  if (PyTuple_GET_SIZE(container_path) < 2) {
    Py_INCREF(module);
    return module;
  }

  PyObject* type_name = PyTuple_GET_ITEM(container_path, 1);
  PyObject* type = _PyClassLoader_GetModuleAttr(module, type_name);
  if (type == Py_None && PyModule_CheckExact(module) &&
      PyModule_GetDict(module) == tstate->interp->builtins) {
    /* special case builtins.None, it's used to represent NoneType */
    Py_DECREF(type);
    type = (PyObject*)Py_TYPE(Py_None);
  }

  if (type != NULL) {
    // Deal with generic and nested types
    for (Py_ssize_t i = 2; i < PyTuple_GET_SIZE(container_path); i++) {
      if (!PyType_Check(type)) {
        break;
      }

      PyObject* type_arg = PyTuple_GET_ITEM(container_path, i);
      if (PyTuple_CheckExact(type_arg)) {
        // Generic type instantation
        PyObject* new_type =
            classloader_instantiate_generic(type, type_arg, Py_None);
        Py_DECREF(type);
        type = new_type;
      } else if (
          PyUnicode_Check(type_arg) &&
          (PyUnicode_CompareWithASCIIString(type_arg, "?") == 0 ||
           PyUnicode_CompareWithASCIIString(type_arg, "#") == 0 ||
           PyUnicode_CompareWithASCIIString(type_arg, "!") == 0)) {
        // Optional, primitive, final specification
        continue;
      } else {
        // Nested type
        PyObject* new_type =
            PyDict_GetItem(_PyType_GetDict((PyTypeObject*)type), type_arg);
        if (new_type == NULL) {
          PyErr_Format(
              CiExc_StaticTypeError,
              "bad name provided for class loader: %R doesn't exist in type "
              "'%R'",
              type_arg,
              type);
          Py_DECREF(type);
          return NULL;
        }
        Py_DECREF(type);
        Py_INCREF(new_type);
        type = new_type;
      }
    }
  }

  return type;
}

/**
    Makes sure the given type is a PyTypeObject (raises an error if not)
*/
int _PyClassLoader_VerifyType(PyObject* type, PyObject* path) {
  if (type == NULL) {
    assert(PyErr_Occurred());
    return -1;
  } else if (!PyType_Check(type)) {
    PyErr_Format(
        CiExc_StaticTypeError,
        "bad name provided for class loader: %R, not a class",
        path);
    return -1;
  }
  return 0;
}

/* Resolve a tuple type descr in the form ("module", "submodule", "Type") to a
 * PyTypeObject*` and `optional` integer out param.
 */
PyTypeObject*
_PyClassLoader_ResolveType(PyObject* descr, int* optional, int* exact) {
  if (!PyTuple_Check(descr) || PyTuple_GET_SIZE(descr) < 2) {
    PyErr_Format(CiExc_StaticTypeError, "unknown type %R", descr);
    return NULL;
  }

  Py_ssize_t items = PyTuple_GET_SIZE(descr);
  PyObject* last = PyTuple_GET_ITEM(descr, items - 1);

  *optional = 0;
  *exact = 0;

  while (PyUnicode_Check(last)) {
    if (PyUnicode_CompareWithASCIIString(last, "?") == 0) {
      *optional = 1;
    } else if (PyUnicode_CompareWithASCIIString(last, "!") == 0) {
      *exact = 1;
    } else if (PyUnicode_CompareWithASCIIString(last, "#") != 0) {
      break;
    } else {
      *exact = 1;
    }
    items--;
    last = PyTuple_GET_ITEM(descr, items - 1);
  }

  if (classloader_cache != NULL) {
    PyObject* cache = PyDict_GetItem(classloader_cache, descr);
    if (cache != NULL) {
      Py_INCREF(cache);
      return (PyTypeObject*)cache;
    }
  }

  PyObject* res = _PyClassLoader_ResolveContainer(descr);
  if (_PyClassLoader_VerifyType(res, descr)) {
    Py_XDECREF(res);
    return NULL;
  }

  if (classloader_cache == NULL) {
    classloader_cache = PyDict_New();
    if (classloader_cache == NULL) {
      Py_DECREF(res);
      return NULL;
    }
  }

  if (classloader_cache_module_to_keys == NULL) {
    classloader_cache_module_to_keys = PyDict_New();
    if (classloader_cache_module_to_keys == NULL) {
      Py_DECREF(res);
      return NULL;
    }
  }

  if (PyDict_SetItem(classloader_cache, descr, res)) {
    Py_DECREF(res);
    return NULL;
  }
  PyObject* module_key = PyTuple_GET_ITEM(descr, 0);
  PyObject* existing_modules_to_keys =
      PyDict_GetItem(classloader_cache_module_to_keys, module_key);
  if (existing_modules_to_keys == NULL) {
    existing_modules_to_keys = PyList_New(0);
    if (existing_modules_to_keys == NULL) {
      Py_DECREF(res);
      return NULL;
    }
    if (PyDict_SetItem(
            classloader_cache_module_to_keys,
            module_key,
            existing_modules_to_keys) < 0) {
      Py_DECREF(res);
      return NULL;
    }
    Py_DECREF(existing_modules_to_keys);
  }
  if (PyList_Append(existing_modules_to_keys, descr) < 0) {
    Py_DECREF(res);
    return NULL;
  }

  return (PyTypeObject*)res;
}

int _PyClassLoader_CheckModuleChange(PyDictObject* dict, PyObject* key) {
  PyThreadState* tstate = PyThreadState_GET();
  PyObject* modules_dict = CI_INTERP_IMPORT_FIELD(tstate->interp, modules);
  if (((PyObject*)dict) != modules_dict) {
    return 0;
  }
  if (classloader_cache_module_to_keys == NULL) {
    return 0;
  }
  PyObject* keys_to_invalidate =
      PyDict_GetItem(classloader_cache_module_to_keys, key);
  if (keys_to_invalidate == NULL) {
    return 0;
  }
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(keys_to_invalidate); i++) {
    PyObject* key_to_invalidate = PyList_GET_ITEM(keys_to_invalidate, i);
    if (PyDict_DelItem(classloader_cache, key_to_invalidate) < 0) {
      return 0;
    }
  }
  PyDict_DelItem(classloader_cache_module_to_keys, key);
  return 0;
}

void _PyClassLoader_ClearCache() {
  Py_CLEAR(classloader_cache);
  Py_CLEAR(classloader_cache_module_to_keys);
}

PyObject* _PyClassLoader_GetCache() {
  if (classloader_cache == NULL) {
    classloader_cache = PyDict_New();
  }
  return classloader_cache;
}

/* Resolve a tuple type descr to a `prim_type` integer (`TYPED_*`); return -1
 * and set an error if the type cannot be resolved. */
int _PyClassLoader_ResolvePrimitiveType(PyObject* descr) {
  if (!PyTuple_Check(descr) || PyTuple_GET_SIZE(descr) < 2) {
    PyErr_Format(CiExc_StaticTypeError, "unknown type %R", descr);
    return -1;
  }

  PyObject* last_elem = PyTuple_GetItem(descr, PyTuple_GET_SIZE(descr) - 1);
  if (!PyUnicode_CheckExact(last_elem) ||
      PyUnicode_CompareWithASCIIString(last_elem, "#") != 0) {
    return TYPED_OBJECT;
  }

  int optional, exact;
  PyTypeObject* type = _PyClassLoader_ResolveType(descr, &optional, &exact);
  if (type == NULL) {
    return -1;
  }
  int res = _PyClassLoader_GetTypeCode(type);
  Py_DECREF(type);
  return res;
}

int is_static_type(PyTypeObject* type) {
  return (type->tp_flags &
          (Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED |
           Ci_Py_TPFLAGS_GENERIC_TYPE_INST)) ||
      !(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
}
