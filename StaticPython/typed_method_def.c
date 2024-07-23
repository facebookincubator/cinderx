/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/typed_method_def.h"

#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/generic_type.h"
#include "cinderx/Upgrade/upgrade_stubs.h"  // @donotremove

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
            CiExc_StaticTypeError, "%U() argument %d is missing", func_name, arg);
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

static int Ci_populate_type_info(PyObject* arg_info, int argtype) {
  _Py_IDENTIFIER(NoneType);
  _Py_IDENTIFIER(object);
  _Py_IDENTIFIER(str);
  _Py_static_string(__static__int8, "__static__.int8");
  _Py_static_string(__static__int16, "__static__.int16");
  _Py_static_string(__static__int32, "__static__.int32");
  _Py_static_string(__static__int64, "__static__.int64");
  _Py_static_string(__static__uint8, "__static__.uint8");
  _Py_static_string(__static__uint16, "__static__.uint16");
  _Py_static_string(__static__uint32, "__static__.uint32");
  _Py_static_string(__static__uint64, "__static__.uint64");
  _Py_IDENTIFIER(optional);
  _Py_IDENTIFIER(type_param);
  _Py_IDENTIFIER(type);

  if ((argtype & Ci_Py_SIG_OPTIONAL) &&
      _PyDict_SetItemId(arg_info, &PyId_optional, Py_True)) {
    return -1;
  }

  if (argtype & Ci_Py_SIG_TYPE_PARAM) {
    /* indicate the type parameter */
    PyObject* type = PyLong_FromLong(Ci_Py_SIG_TYPE_MASK(argtype));
    if (_PyDict_SetItemId(arg_info, &PyId_type_param, type)) {
      Py_DECREF(type);
      return -1;
    }
    Py_DECREF(type);
  } else {
    PyObject* name;
    switch (argtype & ~Ci_Py_SIG_OPTIONAL) {
      case Ci_Py_SIG_ERROR:
      case Ci_Py_SIG_VOID:
        name = _PyUnicode_FromId(&PyId_NoneType);
        break;
      case Ci_Py_SIG_OBJECT:
        name = _PyUnicode_FromId(&PyId_object);
        break;
      case Ci_Py_SIG_STRING:
        name = _PyUnicode_FromId(&PyId_str);
        break;
      case Ci_Py_SIG_INT8:
        name = _PyUnicode_FromId(&__static__int8);
        break;
      case Ci_Py_SIG_INT16:
        name = _PyUnicode_FromId(&__static__int16);
        break;
      case Ci_Py_SIG_INT32:
        name = _PyUnicode_FromId(&__static__int32);
        break;
      case Ci_Py_SIG_INT64:
        name = _PyUnicode_FromId(&__static__int64);
        break;
      case Ci_Py_SIG_UINT8:
        name = _PyUnicode_FromId(&__static__uint8);
        break;
      case Ci_Py_SIG_UINT16:
        name = _PyUnicode_FromId(&__static__uint16);
        break;
      case Ci_Py_SIG_UINT32:
        name = _PyUnicode_FromId(&__static__uint32);
        break;
      case Ci_Py_SIG_UINT64:
        name = _PyUnicode_FromId(&__static__uint64);
        break;
      default:
        PyErr_SetString(PyExc_RuntimeError, "unknown type");
        return -1;
    }
    if (name == NULL || _PyDict_SetItemId(arg_info, &PyId_type, name)) {
      return -1;
    }
  }
  return 0;
}

PyObject* Ci_PyMethodDef_GetTypedSignature(PyMethodDef* method) {
  _Py_IDENTIFIER(default);
  _Py_IDENTIFIER(type);
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
      } else if (_PyDict_SetItemId(arg_info, &PyId_type, name)) {
        Py_DECREF(name);
        Py_DECREF(res);
        return NULL;
      }
      Py_DECREF(name);
    }

    if ((*sig)->se_default_value != NULL &&
        _PyDict_SetItemId(arg_info, &PyId_default, (*sig)->se_default_value)) {
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
