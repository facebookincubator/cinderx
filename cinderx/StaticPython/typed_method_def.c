/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/typed_method_def.h"

#include "cinderx/Common/string.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/generic_type.h"

const Ci_Py_SigElement Ci_Py_Sig_T0 = {Ci_Py_SIG_TYPE_PARAM_IDX(0)};
const Ci_Py_SigElement Ci_Py_Sig_T1 = {Ci_Py_SIG_TYPE_PARAM_IDX(1)};
const Ci_Py_SigElement Ci_Py_Sig_T0_Opt = {
    Ci_Py_SIG_TYPE_PARAM_IDX(0) | Ci_Py_SIG_OPTIONAL,
    Py_None};
const Ci_Py_SigElement Ci_Py_Sig_T1_Opt = {
    Ci_Py_SIG_TYPE_PARAM_IDX(1) | Ci_Py_SIG_OPTIONAL,
    Py_None};
const Ci_Py_SigElement Ci_Py_Sig_Object = {Ci_Py_SIG_OBJECT};
const Ci_Py_SigElement Ci_Py_Sig_Object_Opt = {
    Ci_Py_SIG_OBJECT | Ci_Py_SIG_OPTIONAL,
    Py_None};
const Ci_Py_SigElement Ci_Py_Sig_String = {Ci_Py_SIG_STRING};
const Ci_Py_SigElement Ci_Py_Sig_String_Opt = {
    Ci_Py_SIG_STRING | Ci_Py_SIG_OPTIONAL,
    Py_None};

const Ci_Py_SigElement Ci_Py_Sig_SSIZET = {Ci_Py_SIG_SSIZE_T};
const Ci_Py_SigElement Ci_Py_Sig_SIZET = {Ci_Py_SIG_SIZE_T};
const Ci_Py_SigElement Ci_Py_Sig_INT8 = {Ci_Py_SIG_INT8};
const Ci_Py_SigElement Ci_Py_Sig_INT16 = {Ci_Py_SIG_INT16};
const Ci_Py_SigElement Ci_Py_Sig_INT32 = {Ci_Py_SIG_INT32};
const Ci_Py_SigElement Ci_Py_Sig_INT64 = {Ci_Py_SIG_INT64};
const Ci_Py_SigElement Ci_Py_Sig_UINT8 = {Ci_Py_SIG_UINT8};
const Ci_Py_SigElement Ci_Py_Sig_UINT16 = {Ci_Py_SIG_UINT16};
const Ci_Py_SigElement Ci_Py_Sig_UINT32 = {Ci_Py_SIG_UINT32};
const Ci_Py_SigElement Ci_Py_Sig_UINT64 = {Ci_Py_SIG_UINT64};

#define GENINST_GET_PARAM(self, i) \
  (((_PyGenericTypeInst*)Py_TYPE(self))->gti_inst[i].gtp_type)

void _PyClassLoader_ArgError(
    PyObject* func_name,
    int arg,
    int type_param,
    const Ci_Py_SigElement* sig_elem,
    PyObject* ctx) {
  const char* expected = "?";
  int argtype = sig_elem->se_argtype;
  if (argtype & Ci_Py_SIG_TYPE_PARAM) {
    expected =
        ((PyTypeObject*)GENINST_GET_PARAM(ctx, Ci_Py_SIG_TYPE_MASK(argtype)))
            ->tp_name;

  } else {
    switch (Ci_Py_SIG_TYPE_MASK(argtype)) {
      case Ci_Py_SIG_OBJECT:
        PyErr_Format(
            CiExc_StaticTypeError,
            "%U() argument %d is missing",
            func_name,
            arg);
        return;
      case Ci_Py_SIG_STRING:
        expected = "str";
        break;
      case Ci_Py_SIG_SSIZE_T:
        expected = "int";
        break;
    }
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "%U() argument %d expected %s",
      func_name,
      arg,
      expected);
}

void _PyClassLoader_ArgErrorStr(
    const char* func_name,
    int arg,
    const Ci_Py_SigElement* sig_elem,
    PyObject* ctx) {
  const char* expected = "?";
  int argtype = sig_elem->se_argtype;
  if (argtype & Ci_Py_SIG_TYPE_PARAM) {
    expected =
        ((PyTypeObject*)GENINST_GET_PARAM(ctx, Ci_Py_SIG_TYPE_MASK(argtype)))
            ->tp_name;

  } else {
    switch (Ci_Py_SIG_TYPE_MASK(argtype)) {
      case Ci_Py_SIG_OBJECT:
        PyErr_Format(
            CiExc_StaticTypeError,
            "%s() argument %d is missing",
            func_name,
            arg);
        return;
      case Ci_Py_SIG_STRING:
        expected = "str";
        break;
      case Ci_Py_SIG_SSIZE_T:
        expected = "int";
        break;
    }
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "%s() argument %d expected %s",
      func_name,
      arg,
      expected);
}

int _PyClassLoader_CheckOneArg(
    PyObject* self,
    PyObject* arg,
    char* name,
    int pos,
    const Ci_Py_SigElement* elem) {
  if (arg == Py_None && elem->se_argtype & Ci_Py_SIG_OPTIONAL) {
    return 0;
  }
  _PyGenericTypeParam* param =
      &((_PyGenericTypeInst*)Py_TYPE(self))
           ->gti_inst[Ci_Py_SIG_TYPE_MASK(elem->se_argtype)];

  if (!PyObject_TypeCheck(arg, param->gtp_type)) {
    _PyClassLoader_ArgErrorStr(name, pos + 1, elem, self);
    return -1;
  }
  return 0;
}

static int Ci_populate_type_info(PyObject* arg_info, int argtype) {
  DEFINE_STATIC_STRING(NoneType);
  DEFINE_STATIC_STRING(object);
  DEFINE_STATIC_STRING(optional);
  DEFINE_STATIC_STRING(str);
  DEFINE_STATIC_STRING(type);
  DEFINE_STATIC_STRING(type_param);
  DEFINE_NAMED_STATIC_STRING(s___static__int8, "__static__.int8");
  DEFINE_NAMED_STATIC_STRING(s___static__int16, "__static__.int16");
  DEFINE_NAMED_STATIC_STRING(s___static__int32, "__static__.int32");
  DEFINE_NAMED_STATIC_STRING(s___static__int64, "__static__.int64");
  DEFINE_NAMED_STATIC_STRING(s___static__uint8, "__static__.uint8");
  DEFINE_NAMED_STATIC_STRING(s___static__uint16, "__static__.uint16");
  DEFINE_NAMED_STATIC_STRING(s___static__uint32, "__static__.uint32");
  DEFINE_NAMED_STATIC_STRING(s___static__uint64, "__static__.uint64");

  if ((argtype & Ci_Py_SIG_OPTIONAL) &&
      PyDict_SetItem(arg_info, s_optional, Py_True)) {
    return -1;
  }

  if (argtype & Ci_Py_SIG_TYPE_PARAM) {
    /* indicate the type parameter */
    PyObject* type = PyLong_FromLong(Ci_Py_SIG_TYPE_MASK(argtype));
    if (PyDict_SetItem(arg_info, s_type_param, type)) {
      Py_DECREF(type);
      return -1;
    }
    Py_DECREF(type);
  } else {
    PyObject* name;
    switch (argtype & ~Ci_Py_SIG_OPTIONAL) {
      case Ci_Py_SIG_ERROR:
      case Ci_Py_SIG_VOID:
        name = s_NoneType;
        break;
      case Ci_Py_SIG_OBJECT:
        name = s_object;
        break;
      case Ci_Py_SIG_STRING:
        name = s_str;
        break;
      case Ci_Py_SIG_INT8:
        name = s___static__int8;
        break;
      case Ci_Py_SIG_INT16:
        name = s___static__int16;
        break;
      case Ci_Py_SIG_INT32:
        name = s___static__int32;
        break;
      case Ci_Py_SIG_INT64:
        name = s___static__int64;
        break;
      case Ci_Py_SIG_UINT8:
        name = s___static__uint8;
        break;
      case Ci_Py_SIG_UINT16:
        name = s___static__uint16;
        break;
      case Ci_Py_SIG_UINT32:
        name = s___static__uint32;
        break;
      case Ci_Py_SIG_UINT64:
        name = s___static__uint64;
        break;
      default:
        PyErr_SetString(PyExc_RuntimeError, "unknown type");
        return -1;
    }
    if (name == NULL || PyDict_SetItem(arg_info, s_type, name)) {
      return -1;
    }
  }
  return 0;
}

PyObject* Ci_PyMethodDef_GetTypedSignature(PyMethodDef* method) {
  DEFINE_STATIC_STRING(default);
  DEFINE_STATIC_STRING(type);
  if (!(method->ml_flags & Ci_METH_TYPED)) {
    Py_RETURN_NONE;
  }
  Ci_PyTypedMethodDef* def = (Ci_PyTypedMethodDef*)method->ml_meth;
  PyObject* res = PyDict_New();
  PyObject* args = PyList_New(0);
  if (PyDict_SetItemString(res, "args", args)) {
    Py_DECREF(res);
    return NULL;
  }
  Py_DECREF(args);
  const Ci_Py_SigElement* const* sig = def->tmd_sig;
  while (*sig != NULL) {
    /* each arg is a dictionary */
    PyObject* arg_info = PyDict_New();
    if (arg_info == NULL) {
      Py_DECREF(res);
      return NULL;
    } else if (PyList_Append(args, arg_info)) {
      Py_DECREF(arg_info);
      Py_DECREF(res);
      return NULL;
    }
    Py_DECREF(arg_info); /* kept alive by args list */
    int argtype = (*sig)->se_argtype;
    if (Ci_populate_type_info(arg_info, argtype)) {
      return NULL;
    }

    PyObject* name;
    if ((*sig)->se_name != NULL) {
      name = PyUnicode_FromString((*sig)->se_name);
      if (name == NULL) {
        Py_DECREF(res);
        return NULL;
      } else if (PyDict_SetItem(arg_info, s_type, name)) {
        Py_DECREF(name);
        Py_DECREF(res);
        return NULL;
      }
      Py_DECREF(name);
    }

    if ((*sig)->se_default_value != NULL &&
        PyDict_SetItem(arg_info, s_default, (*sig)->se_default_value)) {
      Py_DECREF(res);
      return NULL;
    }
    sig++;
  }

  PyObject* ret_info = PyDict_New();
  if (ret_info == NULL || PyDict_SetItemString(res, "return", ret_info) ||
      Ci_populate_type_info(ret_info, def->tmd_ret)) {
    Py_XDECREF(ret_info);
    return NULL;
  }
  Py_DECREF(ret_info);

  return res;
}

// Put definition of Ci_static_rand here so that it is accessible from the JIT
// as well as from _static.c
#if PY_VERSION_HEX < 0x030C0000
int64_t Ci_static_rand(PyObject* self) {
  return rand();
}
#else
PyObject* Ci_static_rand(PyObject* self) {
  return PyLong_FromLong(rand());
}
#endif
