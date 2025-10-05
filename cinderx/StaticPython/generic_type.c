// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/generic_type.h"

#include "cinderx/Common/string.h"
#include "cinderx/StaticPython/vtable.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

static PyObject* genericinst_cache;

void _PyClassLoader_ClearGenericTypes() {
  Py_CLEAR(genericinst_cache);
}

static PyObject* get_optional_type(PyObject* type) {
  PyObject* res = NULL;
  PyObject* args = NULL;
  PyObject* origin = NULL;
  PyObject* name = NULL;

  if (!PyType_Check(type)) {
    DEFINE_STATIC_STRING(__args__);
    DEFINE_STATIC_STRING(__origin__);
    DEFINE_STATIC_STRING(_name);

    args = PyObject_GetAttr(type, s___args__);
    if (args == NULL) {
      PyErr_Clear();
      goto done;
    } else if (!PyTuple_CheckExact(args) || PyTuple_GET_SIZE(args) != 2) {
      goto done;
    }

    if (Py_TYPE(type) != Cix_PyUnion_Type) {
      origin = PyObject_GetAttr(type, s___origin__);
      if (origin == NULL) {
        PyErr_Clear();
        goto done;
      } else if (strcmp(Py_TYPE(origin)->tp_name, "_SpecialForm")) {
        goto done;
      }

      name = PyObject_GetAttr(origin, s__name);
      if (name == NULL) {
        PyErr_Clear();
        goto done;
      }
      if (!PyUnicode_CheckExact(name) ||
          PyUnicode_CompareWithASCIIString(name, "Union") != 0) {
        goto done;
      }
    }

    PyObject* one = PyTuple_GET_ITEM(args, 0);
    PyObject* two = PyTuple_GET_ITEM(args, 1);
    if (PyType_Check(one) &&
        (two == (PyObject*)Py_TYPE(Py_None) || two == Py_None)) {
      Py_INCREF(one);
      res = one;
    } else if (
        PyType_Check(two) &&
        (one == (PyObject*)Py_TYPE(Py_None) || one == Py_None)) {
      Py_INCREF(two);
      res = two;
    }
  }

done:
  Py_XDECREF(args);
  Py_XDECREF(origin);
  Py_XDECREF(name);
  return res;
}

int gtd_validate_type(PyObject* type, PyObject** args, Py_ssize_t nargs) {
  /* We don't allow subclassing from generic classes yet */
  assert(!(((PyTypeObject*)type)->tp_flags & Py_TPFLAGS_BASETYPE));
  /* Can't create instances of generic types */
  assert(((PyTypeObject*)type)->tp_new == NULL);

  _PyGenericTypeDef* def = (_PyGenericTypeDef*)type;
  if (nargs != def->gtd_size) {
    PyErr_Format(
        PyExc_TypeError,
        "%s expected %d generic arguments, got %d",
        ((PyTypeObject*)type)->tp_name,
        def->gtd_size,
        nargs);
    return -1;
  }
  for (Py_ssize_t i = 0; i < nargs; i++) {
    if (!PyType_Check(args[i])) {
      PyObject* opt = get_optional_type(args[i]);
      if (opt == NULL) {
        PyErr_SetString(
            PyExc_TypeError,
            "expected type or Optional[T] for generic argument");
        return -1;
      }
      Py_DECREF(opt);
    }
  }
  return 0;
}

PyObject* gtd_make_key(PyObject* type, PyObject** args, Py_ssize_t nargs) {
  PyObject* key = PyTuple_New(nargs + 1);
  if (key == NULL) {
    return NULL;
  }
  PyTuple_SET_ITEM(key, 0, type);
  Py_INCREF(type);
  for (Py_ssize_t i = 0; i < nargs; i++) {
    PyTuple_SET_ITEM(key, i + 1, args[i]);
    Py_INCREF(args[i]);
  }
  return key;
}

void geninst_dealloc(PyObject* obj) {
  /* these are heap types, so we need to decref their type.  We delegate
   * to the generic type definitions deallocator, and then dec ref the type
   * here */
  PyTypeObject* inst_type = Py_TYPE(obj);
  ((PyTypeObject*)((_PyGenericTypeInst*)inst_type)->gti_gtd)->tp_dealloc(obj);
  Py_DECREF(inst_type);
}

PyObject* gti_calc_name(PyObject* type, _PyGenericTypeInst* new_inst) {
  Py_ssize_t nargs = new_inst->gti_size;
  const char* orig_name = ((PyTypeObject*)type)->tp_name;
  const char* dot;
  if ((dot = strchr(orig_name, '.')) != NULL) {
    orig_name = dot + 1;
  }
  char* start = strchr(orig_name, '[');
  assert(start != NULL);

  Py_ssize_t len = strlen(orig_name);
  for (int i = 0; i < nargs; i++) {
    PyTypeObject* param_type = new_inst->gti_inst[i].gtp_type;
    len += strlen(param_type->tp_name);
    if (new_inst->gti_inst[i].gtp_optional) {
      len += strlen("Optional[]");
    }
    len += 2;
  }

  char buf[len];
  strncpy(buf, orig_name, start - orig_name + 1);
  buf[start - orig_name + 1] = 0;
  for (int i = 0; i < nargs; i++) {
    PyTypeObject* param_type = new_inst->gti_inst[i].gtp_type;
    if (i != 0) {
      strcat(buf, ", ");
    }
    if (new_inst->gti_inst[i].gtp_optional) {
      strcat(buf, "Optional[");
    }
    strcat(buf, param_type->tp_name);
    if (new_inst->gti_inst[i].gtp_optional) {
      strcat(buf, "]");
    }
  }
  strcat(buf, "]");
  return PyUnicode_FromString(buf);
}

int set_module_name(_PyGenericTypeDef* type, PyTypeObject* new_type) {
  PyObject* mod;
  const char* base_name = ((PyTypeObject*)type)->tp_name;
  const char* s = strrchr(base_name, '.');
  DEFINE_STATIC_STRING(__module__);
  DEFINE_STATIC_STRING(builtins);

  if (s != NULL) {
    mod = PyUnicode_FromStringAndSize(base_name, (Py_ssize_t)(s - base_name));
    if (mod != NULL) {
      PyUnicode_InternInPlace(&mod);
    }
  } else {
    mod = s_builtins;
  }
  if (mod == NULL) {
    return -1;
  }
  int result = PyDict_SetItem(
      _PyType_GetDict(((PyTypeObject*)new_type)), s___module__, mod);
  Py_DECREF(mod);
  return result;
}

PyTypeObject* gtd_make_heap_type(PyTypeObject* type, Py_ssize_t size) {
#if PY_VERSION_HEX < 0x030C0000
  Py_ssize_t basicsize = _Py_SIZE_ROUND_UP(size, SIZEOF_VOID_P);
  PyTypeObject* new_type = (PyTypeObject*)_PyObject_GC_Calloc(basicsize);
  if (new_type == NULL) {
    return NULL;
  }

  PyObject_INIT_VAR(new_type, &PyType_Type, 0);
#else
  PyTypeObject* new_type =
      (PyTypeObject*)PyUnstable_Object_GC_NewWithExtraData(&PyType_Type, size);
  if (new_type == NULL) {
    return NULL;
  }
#endif

  /* Copy the generic def into the instantiation */
#define COPY_DATA(name) new_type->name = type->name;
  COPY_DATA(tp_name);
  COPY_DATA(tp_basicsize);
  COPY_DATA(tp_dealloc);
  COPY_DATA(tp_itemsize);
  COPY_DATA(tp_vectorcall_offset);
  COPY_DATA(tp_getattr);
  COPY_DATA(tp_setattr);
  COPY_DATA(tp_as_async);
  COPY_DATA(tp_repr);
  COPY_DATA(tp_as_number);
  COPY_DATA(tp_as_sequence);
  COPY_DATA(tp_as_mapping);
  COPY_DATA(tp_hash);
  COPY_DATA(tp_call);
  COPY_DATA(tp_str);
  COPY_DATA(tp_getattro);
  COPY_DATA(tp_setattro);
  COPY_DATA(tp_as_buffer);
  COPY_DATA(tp_flags);
  COPY_DATA(tp_traverse);
  COPY_DATA(tp_clear);
  COPY_DATA(tp_richcompare);
  COPY_DATA(tp_weaklistoffset);
  COPY_DATA(tp_iter);
  COPY_DATA(tp_iternext);
  COPY_DATA(tp_methods);
  COPY_DATA(tp_members);
  COPY_DATA(tp_getset);
  COPY_DATA(tp_base);
  Py_XINCREF(new_type->tp_base);
  COPY_DATA(tp_descr_get);
  COPY_DATA(tp_descr_set);
  COPY_DATA(tp_dictoffset);
  COPY_DATA(tp_init);
  COPY_DATA(tp_alloc);
  COPY_DATA(tp_new);
  COPY_DATA(tp_free);
  if (type->tp_doc != NULL) {
    // tp_doc is heap allocated, so we need to copy it.
    size_t len = strlen(type->tp_doc) + 1;
#if PY_VERSION_HEX >= 0x030E0000
    char* new_doc = PyMem_Malloc(len);
#else
    char* new_doc = PyObject_Malloc(len);
#endif
    if (new_doc == NULL) {
      goto error;
    }
    memcpy(new_doc, type->tp_doc, len);
    new_type->tp_doc = new_doc;
  }
  new_type->tp_new = ((_PyGenericTypeDef*)type)->gtd_new;
#undef COPY_DATA

#if PY_VERSION_HEX < 0x030C0000
  new_type->tp_flags |= Py_TPFLAGS_HEAPTYPE | Ci_Py_TPFLAGS_FROZEN;
#else
  new_type->tp_flags |= Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_IMMUTABLETYPE;
#endif
  new_type->tp_flags &= ~Py_TPFLAGS_READY;
  PyObject_GC_Track(new_type);
  return new_type;
error:
  Py_XDECREF(new_type);
  return NULL;
}

PyTypeObject* _PyClassLoader_MakeGenericHeapType(_PyGenericTypeDef* type) {
  if (strchr(type->gtd_type.ht_type.tp_name, '.') == NULL) {
    PyErr_Format(
        PyExc_ValueError,
        "type needs to have module %s",
        type->gtd_type.ht_type.tp_name);
    return NULL;
  }
  _PyGenericTypeDef* new_type = (_PyGenericTypeDef*)gtd_make_heap_type(
      (PyTypeObject*)&type->gtd_type, sizeof(_PyGenericTypeDef));
  assert(new_type != type);
  if (new_type == NULL || PyType_Ready((PyTypeObject*)new_type) < 0) {
    Py_XDECREF(new_type);
    return NULL;
  }

  _PyGenericTypeDef* gen_type = (_PyGenericTypeDef*)type;
  new_type->gtd_new = gen_type->gtd_new;
  new_type->gtd_size = gen_type->gtd_size;
  /* can't create generic types */
  new_type->gtd_type.ht_type.tp_new = NULL;
  new_type->gtd_type.ht_name =
      PyUnicode_FromString(strchr(type->gtd_type.ht_type.tp_name, '.') + 1);
  new_type->gtd_type.ht_qualname =
      PyUnicode_FromString(type->gtd_type.ht_type.tp_name);

  if (set_module_name(type, (PyTypeObject*)new_type)) {
    Py_DECREF(new_type);
    return NULL;
  }

  return (PyTypeObject*)new_type;
}

PyObject* gtd_new_inst(PyObject* type, PyObject** args, Py_ssize_t nargs) {
  /* We have to allocate this in a very strange way, as we want the
   * extra space for a _PyGenericTypeInst, along with the generic
   * arguments.  But the type can't have a non-zero Py_SIZE (which would
   * be for PyHeapTypeObject's PyMemberDef's).  So we calculate the
   * size by hand.  This is currently fine as we don't support subclasses
   * of generic types. */
  Py_ssize_t extra_size =
      sizeof(_PyGenericTypeInst) + sizeof(_PyGenericTypeParam) * nargs;
  _PyGenericTypeInst* new_inst =
      (_PyGenericTypeInst*)gtd_make_heap_type((PyTypeObject*)type, extra_size);
  PyTypeObject* new_type = (PyTypeObject*)new_inst;

  new_inst->gti_type.ht_type.tp_dealloc = geninst_dealloc;
  new_inst->gti_type.ht_type.tp_flags |= Ci_Py_TPFLAGS_GENERIC_TYPE_INST;
  new_inst->gti_type.ht_type.tp_flags &= ~(Ci_Py_TPFLAGS_GENERIC_TYPE_DEF);

  new_inst->gti_gtd = (_PyGenericTypeDef*)type;
  Py_INCREF(type);

  new_inst->gti_size = nargs;

  // The lifetime of the generic type parameters is managed by the vtable
  _PyType_GenericTypeRef* gtr =
      PyMem_Malloc(sizeof(_PyType_GenericTypeRef) + sizeof(PyObject*) * nargs);
  if (gtr == NULL) {
    goto error;
  }
  gtr->gtr_gtd = type;
  gtr->gtr_typeparam_count = nargs;
  for (int i = 0; i < nargs; i++) {
    PyObject* opt_type = get_optional_type(args[i]);
    if (opt_type == NULL) {
      new_inst->gti_inst[i].gtp_type = (PyTypeObject*)args[i];
      Py_INCREF(args[i]);
      new_inst->gti_inst[i].gtp_optional = 0;
    } else {
      new_inst->gti_inst[i].gtp_type = (PyTypeObject*)opt_type;
      new_inst->gti_inst[i].gtp_optional = 1;
    }
    gtr->gtr_typeparams[i] = new_inst->gti_inst[i].gtp_type;
  }

  PyObject* name = gti_calc_name(type, new_inst);
  if (name == NULL) {
    goto error;
  }

  new_inst->gti_type.ht_name = name;
  new_inst->gti_type.ht_qualname = name;
  Py_INCREF(name);
  Py_ssize_t name_size;
  new_inst->gti_type.ht_type.tp_name =
      PyUnicode_AsUTF8AndSize(name, &name_size);

  if (new_inst->gti_type.ht_type.tp_name == NULL ||
      PyType_Ready((PyTypeObject*)new_inst)) {
    goto error;
  }

  _PyType_VTable* vtable =
      _PyClassLoader_EnsureVtable((PyTypeObject*)new_inst, 0);
  if (vtable == NULL) {
    goto error;
  }

  vtable->vt_gtr = gtr;
  if (new_type->tp_base != NULL) {
    new_type->tp_new = new_type->tp_base->tp_new;
  }
  return (PyObject*)new_inst;
error:
  if (gtr != NULL) {
    PyMem_Free(gtr);
  }
  Py_DECREF(new_inst);
  return (PyObject*)new_inst;
}

PyObject* _PyClassLoader_GetGenericInst(
    PyObject* type,
    PyObject** args,
    Py_ssize_t nargs) {
  if (genericinst_cache == NULL) {
    genericinst_cache = PyDict_New();
    if (genericinst_cache == NULL) {
      return NULL;
    }
  }

  PyObject* key = gtd_make_key(type, args, nargs);
  if (key == NULL) {
    return NULL;
  }

  PyObject* inst = PyDict_GetItem(genericinst_cache, key);
  if (inst != NULL) {
    Py_DECREF(key);
    Py_INCREF(inst);
    return inst;
  }

  PyObject* res;
  if (!PyType_Check(type)) {
    Py_DECREF(key);
    PyErr_Format(PyExc_TypeError, "expected type, not %R", type);
    return NULL;
  } else if (((PyTypeObject*)type)->tp_flags & Ci_Py_TPFLAGS_GENERIC_TYPE_DEF) {
    if (gtd_validate_type(type, args, nargs)) {
      Py_DECREF(key);
      return NULL;
    }
    res = gtd_new_inst(type, args, nargs);
  } else {
    if (nargs == 1) {
      res = PyObject_GetItem(type, args[0]);
    } else {
      PyObject* argstuple = Cix_PyTuple_FromArray(args, nargs);
      if (argstuple == NULL) {
        Py_DECREF(key);
        return NULL;
      }
      res = PyObject_GetItem(type, argstuple);
      Py_DECREF(argstuple);
    }
  }

  if (res == NULL || PyDict_SetItem(genericinst_cache, key, res)) {
    Py_XDECREF(res);
    Py_DECREF(key);
    return NULL;
  }
  Py_DECREF(key);
  return res;
}

PyObject* _PyClassLoader_GtdGetItem(_PyGenericTypeDef* type, PyObject* args) {
  assert(PyTuple_Check(args));
  if (PyTuple_GET_SIZE(args) != 1) {
    PyErr_SetString(PyExc_TypeError, "expected exactly one argument");
    return NULL;
  }
  args = PyTuple_GET_ITEM(args, 0);
  PyObject* res;
  if (PyTuple_Check(args)) {
    res = _PyClassLoader_GetGenericInst(
        (PyObject*)type,
        ((PyTupleObject*)args)->ob_item,
        PyTuple_GET_SIZE(args));
  } else {
    res = _PyClassLoader_GetGenericInst((PyObject*)type, &args, 1);
  }
  if (res == NULL) {
    return NULL;
  } else if (set_module_name(type, (PyTypeObject*)res) < 0) {
    Py_DECREF(res);
    return NULL;
  }

  return res;
}
