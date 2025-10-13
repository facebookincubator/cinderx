/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include "cinderx/python.h"

#include "cinderx/StaticPython/generic_type.h"
#include "cinderx/StaticPython/type_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flag marks this as optional */
#define Ci_Py_SIG_OPTIONAL 0x01
/* Flag marks this a type param, high bits are type index */
#define Ci_Py_SIG_TYPE_PARAM 0x02
#define Ci_Py_SIG_TYPE_PARAM_IDX(x) ((x << 2) | Ci_Py_SIG_TYPE_PARAM)
#define Ci_Py_SIG_TYPE_PARAM_OPT(x) \
  ((x << 2) | Ci_Py_SIG_TYPE_PARAM | Ci_Py_SIG_OPTIONAL)

typedef struct Ci_Py_SigElement {
  int se_argtype;
  PyObject* se_default_value;
  const char* se_name;
} Ci_Py_SigElement;

typedef struct {
  void* tmd_meth; /* The C function that implements it */
  const Ci_Py_SigElement* const* tmd_sig; /* The function signature */
  int tmd_ret;
} Ci_PyTypedMethodDef;

#define Ci_Py_TYPED_SIGNATURE(name, ret_type, ...)                   \
  static const Ci_Py_SigElement* const name##_sig[] = {__VA_ARGS__}; \
  static Ci_PyTypedMethodDef name##_def = {                          \
      name,                                                          \
      name##_sig,                                                    \
      ret_type,                                                      \
  }

PyObject* Ci_PyMethodDef_GetTypedSignature(PyMethodDef* method);

#define Ci_Py_SIG_INT8 (TYPED_INT8 << 2)
#define Ci_Py_SIG_INT16 (TYPED_INT16 << 2)
#define Ci_Py_SIG_INT32 (TYPED_INT32 << 2)
#define Ci_Py_SIG_INT64 (TYPED_INT64 << 2)
#define Ci_Py_SIG_UINT8 (TYPED_UINT8 << 2)
#define Ci_Py_SIG_UINT16 (TYPED_UINT16 << 2)
#define Ci_Py_SIG_UINT32 (TYPED_UINT32 << 2)
#define Ci_Py_SIG_UINT64 (TYPED_UINT64 << 2)
#define Ci_Py_SIG_OBJECT (TYPED_OBJECT << 2)
#define Ci_Py_SIG_VOID (TYPED_VOID << 2)
#define Ci_Py_SIG_STRING (TYPED_STRING << 2)
#define Ci_Py_SIG_ERROR (TYPED_ERROR << 2)
#define Ci_Py_SIG_SSIZE_T \
  (sizeof(void*) == 8 ? Ci_Py_SIG_INT64 : Ci_Py_SIG_INT32)
#define Ci_Py_SIG_SIZE_T \
  (sizeof(void*) == 8 ? Ci_Py_SIG_UINT64 : Ci_Py_SIG_UINT32)
#define Ci_Py_SIG_TYPE_MASK(x) ((x) >> 2)

extern const Ci_Py_SigElement Ci_Py_Sig_T0;
extern const Ci_Py_SigElement Ci_Py_Sig_T1;
extern const Ci_Py_SigElement Ci_Py_Sig_T0_Opt;
extern const Ci_Py_SigElement Ci_Py_Sig_T1_Opt;
extern const Ci_Py_SigElement Ci_Py_Sig_Object;
extern const Ci_Py_SigElement Ci_Py_Sig_Object_Opt;
extern const Ci_Py_SigElement Ci_Py_Sig_String;
extern const Ci_Py_SigElement Ci_Py_Sig_String_Opt;

extern const Ci_Py_SigElement Ci_Py_Sig_SSIZET;
extern const Ci_Py_SigElement Ci_Py_Sig_SIZET;
extern const Ci_Py_SigElement Ci_Py_Sig_INT8;
extern const Ci_Py_SigElement Ci_Py_Sig_INT16;
extern const Ci_Py_SigElement Ci_Py_Sig_INT32;
extern const Ci_Py_SigElement Ci_Py_Sig_INT64;
extern const Ci_Py_SigElement Ci_Py_Sig_UINT8;
extern const Ci_Py_SigElement Ci_Py_Sig_UINT16;
extern const Ci_Py_SigElement Ci_Py_Sig_UINT32;
extern const Ci_Py_SigElement Ci_Py_Sig_UINT64;

static inline int
_PyClassLoader_CheckParamType(PyObject* self, PyObject* arg, int index) {
  _PyGenericTypeParam* param =
      &((_PyGenericTypeInst*)Py_TYPE(self))->gti_inst[index];
  return (arg == Py_None && param->gtp_optional) ||
      PyObject_TypeCheck(arg, param->gtp_type);
}

int _PyClassLoader_CheckOneArg(
    PyObject* self,
    PyObject* arg,
    char* name,
    int pos,
    const Ci_Py_SigElement* elem);

static inline int
_PyClassLoader_OverflowCheck(PyObject* arg, int type, size_t* value) {
  static uint64_t overflow_masks[] = {
      0xFFFFFFFFFFFFFF00, 0xFFFFFFFFFFFF0000, 0xFFFFFFFF00000000, 0x0};
  static uint64_t soverflow_masks[] = {
      0xFFFFFFFFFFFFFF80,
      0xFFFFFFFFFFFF8000,
      0xFFFFFFFF80000000,
      0x8000000000000000};

  assert(PyLong_Check(arg));

  if (type & TYPED_INT_SIGNED) {
    Py_ssize_t ival = PyLong_AsSsize_t(arg);
    if (ival == -1 && PyErr_Occurred()) {
      PyErr_Clear();
      return 0;
    }
    if ((ival & soverflow_masks[type >> 1]) &&
        (ival & soverflow_masks[type >> 1]) != soverflow_masks[type >> 1]) {
      return 0;
    }
    *value = (size_t)ival;
  } else {
    size_t ival = PyLong_AsSize_t(arg);
    if (ival == (size_t)-1 && PyErr_Occurred()) {
      PyErr_Clear();
      return 0;
    }

    if (ival & overflow_masks[type >> 1]) {
      return 0;
    }
    *value = (size_t)ival;
  }
  return 1;
}

// alias for _PyClassLoader_OverflowCheck to use in the interpreter so it's
// not seen as non-escaping.
static inline int
_PyClassLoader_CheckOverflow(PyObject* arg, int type, size_t* value) {
  return _PyClassLoader_OverflowCheck(arg, type, value);
}

static inline void* _PyClassLoader_ConvertArg(
    PyObject* ctx,
    const Ci_Py_SigElement* sig_elem,
    Py_ssize_t i,
    Py_ssize_t nargsf,
    PyObject* const* args,
    int* error) {
  PyObject* arg =
      i < PyVectorcall_NARGS(nargsf) ? args[i] : sig_elem->se_default_value;
  int argtype = sig_elem->se_argtype;
  if ((argtype & Ci_Py_SIG_OPTIONAL) && (arg == NULL || arg == Py_None)) {
    return arg;
  } else if (arg == NULL) {
    *error = 1;
  } else if (argtype & Ci_Py_SIG_TYPE_PARAM) {
    if (!_PyClassLoader_CheckParamType(
            ctx, arg, Ci_Py_SIG_TYPE_MASK(argtype))) {
      *error = 1;
    }
    return arg;
  } else {
    switch (argtype & ~(Ci_Py_SIG_OPTIONAL)) {
      case Ci_Py_SIG_OBJECT:
        return arg;
      case Ci_Py_SIG_STRING:
        *error = !PyUnicode_Check(arg);
        return arg;
      case Ci_Py_SIG_UINT8:
      case Ci_Py_SIG_UINT16:
      case Ci_Py_SIG_UINT32:
      case Ci_Py_SIG_INT8:
      case Ci_Py_SIG_INT16:
      case Ci_Py_SIG_INT32:
        if (PyLong_Check(arg)) {
          size_t res;
          if (_PyClassLoader_OverflowCheck(
                  arg, Ci_Py_SIG_TYPE_MASK(argtype), &res)) {
            return (void*)res;
          }
          *error = 1;
          PyErr_SetString(PyExc_OverflowError, "overflow");
        } else {
          *error = 1;
        }
        break;
      case Ci_Py_SIG_INT64:
        if (PyLong_Check(arg)) {
          Py_ssize_t val = PyLong_AsSsize_t(arg);
          if (val == -1 && PyErr_Occurred()) {
            *error = 1;
          }
          return (void*)val;
        } else {
          *error = 1;
        }
        break;
      case Ci_Py_SIG_UINT64:
        if (PyLong_Check(arg)) {
          size_t val = PyLong_AsSize_t(arg);
          if (val == ((size_t)-1) && PyErr_Occurred()) {
            *error = 1;
          }
          return (void*)val;
        } else {
          *error = 1;
        }
        break;
    }
  }
  return NULL;
}

static inline PyObject* _PyClassLoader_ConvertRet(void* value, int ret_type) {
  switch (ret_type) {
    // We could update the compiler so that void returning functions either
    // are only used in void contexts, or explicitly emit a LOAD_CONST None
    // when not used in a void context. For now we just produce None here (and
    // in JIT HIR builder).
    case Ci_Py_SIG_VOID:
      Py_INCREF(Py_None);
      return Py_None;
    case Ci_Py_SIG_INT8:
      return PyLong_FromSsize_t((int8_t)(Py_ssize_t)value);
    case Ci_Py_SIG_INT16:
      return PyLong_FromSsize_t((int16_t)(Py_ssize_t)value);
    case Ci_Py_SIG_INT32:
      return PyLong_FromSsize_t((int32_t)(Py_ssize_t)value);
    case Ci_Py_SIG_INT64:
#if SIZEOF_VOID_P >= 8
      return PyLong_FromSsize_t((int64_t)value);
#elif SIZEOF_LONG_LONG < SIZEOF_VOID_P
#error "sizeof(long long) < sizeof(void*)"
#else
      return PyLong_FromLongLong((long long)value);
#endif
    case Ci_Py_SIG_UINT8:
      return PyLong_FromSize_t((uint8_t)(size_t)value);
    case Ci_Py_SIG_UINT16:
      return PyLong_FromSize_t((uint16_t)(size_t)value);
    case Ci_Py_SIG_UINT32:
      return PyLong_FromSize_t((uint32_t)(size_t)value);
    case Ci_Py_SIG_UINT64:
#if SIZEOF_VOID_P >= 8
      return PyLong_FromSize_t((uint64_t)value);
#elif SIZEOF_LONG_LONG < SIZEOF_VOID_P
#error "sizeof(long long) < sizeof(void*)"
#else
      return PyLong_FromUnsignedLongLong((unsigned long long)value);
#endif
    case Ci_Py_SIG_ERROR:
      if (value) {
        return NULL;
      }
      Py_INCREF(Py_None);
      return Py_None;
    default:
      return (PyObject*)value;
  }
}

void _PyClassLoader_ArgError(
    PyObject* func_name,
    int arg,
    int type_param,
    const Ci_Py_SigElement* sig_elem,
    PyObject* ctx);

void _PyClassLoader_ArgErrorStr(
    const char* func_name,
    int arg,
    const Ci_Py_SigElement* sig_elem,
    PyObject* ctx);

static inline PyMethodDef* _PyClassLoader_GetMethodDef(PyObject* obj) {
  if (obj == NULL) {
    return NULL;
  } else if (PyCFunction_Check(obj)) {
    return ((PyCFunctionObject*)obj)->m_ml;
  } else if (Py_TYPE(obj) == &PyMethodDescr_Type) {
    return ((PyMethodDescrObject*)obj)->d_method;
  }
  return NULL;
}

static inline Ci_PyTypedMethodDef* _PyClassLoader_GetTypedMethodDef(
    PyObject* obj) {
  PyMethodDef* def = _PyClassLoader_GetMethodDef(obj);
  if (def && def->ml_flags & Ci_METH_TYPED) {
    return (Ci_PyTypedMethodDef*)def->ml_meth;
  }
  return NULL;
}

static inline int _PyClassLoader_IsStaticBuiltin(PyObject* obj) {
  return (_PyClassLoader_GetTypedMethodDef(obj) != NULL);
}

// Put definition of Ci_static_rand here so that it is accessible from the JIT
// as well as from _static.c
#if PY_VERSION_HEX < 0x030C0000
int64_t Ci_static_rand(PyObject* self);
#else
PyObject* Ci_static_rand(PyObject* self);
#endif

#ifdef __cplusplus
}
#endif
