// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * Utilities to smooth out portability between base runtime versions.
 */

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_interpframe.h"
#endif

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_genobject.h"
#endif

#if PY_VERSION_HEX < 0x030C0000
#define CI_INTERP_IMPORT_FIELD(interp, field) interp->field
#else
#define CI_INTERP_IMPORT_FIELD(interp, field) interp->imports.field
#endif

#include "internal/pycore_interp.h"

#if PY_VERSION_HEX < 0x030C0000
#define _PyType_GetDict(type) ((type)->tp_dict)
#define _PyObject_CallNoArgs _PyObject_CallNoArg
#endif

#if PY_VERSION_HEX < 0x030D0000

// Basic renames that went into 3.13.
#define PyLong_AsInt _PyLong_AsInt
#define PyObject_GetOptionalAttr _PyObject_LookupAttr
#define PyTime_AsSecondsDouble _PyTime_AsSecondsDouble
#define PyTime_t _PyTime_t
#define Py_IsFinalizing _Py_IsFinalizing
#define _PyEval_MatchClass Cix_match_class
#define _PyEval_MatchKeys Cix_match_keys
#define _PyEval_FormatKwargsError Cix_format_kwargs_error
#define _PyEval_FormatExcCheckArg Cix_format_exc_check_arg

#define _PyFrame_GetCode(F) ((F)->f_code)

inline int PyList_Extend(PyObject* list, PyObject* iterable) {
  if (!PyList_Check(list)) {
    PyErr_BadInternalCall();
    return -1;
  }
  PyObject* result = _PyList_Extend((PyListObject*)list, iterable);
  return result != NULL ? 0 : -1;
}

static inline int PyTime_MonotonicRaw(PyTime_t* result) {
  *result = _PyTime_GetMonotonicClock();
  return 0;
}

#endif

#if PY_VERSION_HEX < 0x030E0000

// Basic renames that went into 3.14.
#define _PyGen_GetGeneratorFromFrame _PyFrame_GetGenerator

#endif

#if PY_VERSION_HEX >= 0x030C0000

// Fetch a _PyInterpreterFrame from a PyThreadState.
inline _PyInterpreterFrame* interpFrameFromThreadState(PyThreadState* tstate) {
#if PY_VERSION_HEX >= 0x030D0000
  return tstate->current_frame;
#else
  return tstate->cframe->current_frame;
#endif
}

// Get the interpreter frame stored in a generator object.
inline _PyInterpreterFrame* generatorFrame(PyGenObject* gen) {
  return
#if PY_VERSION_HEX >= 0x030E0000
      &gen->gi_iframe
#else
      (_PyInterpreterFrame*)(gen->gi_iframe)
#endif
      ;
}

// Get the current interpreter frame from a thread state.
inline _PyInterpreterFrame* currentFrame(PyThreadState* tstate) {
#if PY_VERSION_HEX >= 0x030D0000
  return tstate->current_frame;
#else
  return tstate->cframe->current_frame;
#endif
}

// Set the current interpreter frame in a thread state.
inline void setCurrentFrame(PyThreadState* tstate, _PyInterpreterFrame* frame) {
#if PY_VERSION_HEX >= 0x030D0000
  tstate->current_frame = frame;
#else
  tstate->cframe->current_frame = frame;
#endif
}

inline PyObject* frameFunction(_PyInterpreterFrame* frame) {
#if PY_VERSION_HEX >= 0x030E0000
  return PyStackRef_AsPyObjectBorrow(frame->f_funcobj);
#else
  return frame->f_funcobj;
#endif
}

static inline void setFrameFunction(
    _PyInterpreterFrame* frame,
    PyObject* func) {
#if PY_VERSION_HEX >= 0x030E0000
  if (func != NULL) {
    frame->f_funcobj = PyStackRef_FromPyObjectSteal(func);
  } else {
    frame->f_funcobj = PyStackRef_NULL;
  }
#else
  frame->f_funcobj = (PyObject*)func;
#endif
}

inline void setFrameInstruction(_PyInterpreterFrame* frame, _Py_CODEUNIT* loc) {
#if PY_VERSION_HEX >= 0x030E0000
  frame->instr_ptr = loc;
#else
  frame->prev_instr = loc;
#endif
}

#endif // PY_VERSION_HEX >= 0x030C0000

#if PY_VERSION_HEX >= 0x030E0000
#define _CiArg_UnpackKeywords(                                        \
    args, nargs, kwargs, kwnames, parser, minpos, maxpos, minkw, buf) \
  _PyArg_UnpackKeywords(                                              \
      args,                                                           \
      nargs,                                                          \
      kwargs,                                                         \
      kwnames,                                                        \
      parser,                                                         \
      minpos,                                                         \
      maxpos,                                                         \
      minkw,                                                          \
      0 /* varpos */,                                                 \
      buf)
#else
#define _CiArg_UnpackKeywords _PyArg_UnpackKeywords
#endif

#if PY_VERSION_HEX >= 0x030E0000
#define FRAME_EXECUTABLE f_executable
#define FRAME_EXECUTABLE_OFFSET offsetof(_PyInterpreterFrame, f_executable)
#define FRAME_INSTR instr_ptr
#define FRAME_INSTR_OFFSET offsetof(_PyInterpreterFrame, instr_ptr)

inline PyCodeObject* frameCode(_PyInterpreterFrame* frame) {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  PyObject* executable = PyStackRef_AsPyObjectBorrow(frame->f_executable);
  if (PyUnstable_JITExecutable_Check(executable)) {
    return ((PyUnstable_PyJitExecutable*)executable)->je_code;
  }
#endif
  return (PyCodeObject*)PyStackRef_AsPyObjectBorrow(frame->f_executable);
}

inline void setFrameCode(_PyInterpreterFrame* frame, PyObject* code) {
  frame->f_executable = PyStackRef_FromPyObjectNew(code);
}
#elif PY_VERSION_HEX >= 0x30C0000
#define FRAME_EXECUTABLE f_code
#define FRAME_EXECUTABLE_OFFSET offsetof(_PyInterpreterFrame, f_code)
#define FRAME_INSTR prev_instr
#define FRAME_INSTR_OFFSET offsetof(_PyInterpreterFrame, prev_instr)

inline PyCodeObject* frameCode(_PyInterpreterFrame* frame) {
  return frame->f_code;
}
inline void setFrameCode(_PyInterpreterFrame* frame, PyObject* code) {
  frame->f_code = (PyCodeObject*)Py_NewRef(code);
}
#else
inline PyCodeObject* frameCode(PyFrameObject* frame) {
  return frame->f_code;
}
#endif

#if PY_VERSION_HEX >= 0x030C0000
inline PyObject* frameExecutable(_PyInterpreterFrame* frame) {
#if PY_VERSION_HEX >= 0x030E0000
  return PyStackRef_AsPyObjectBorrow(frame->f_executable);
#else
  return (PyObject*)frameCode(frame);
#endif
}
#endif

// Code object flag that will prevent JIT compilation.
//
// Originally defined in Meta Python headers, now defined here as well.
#ifndef CI_CO_SUPPRESS_JIT
#define CI_CO_SUPPRESS_JIT 0x40000000
#endif

// Stack ref compatibility helpers between for 3.14+
#if PY_VERSION_HEX < 0x030E0000

#define Ci_STACK_TYPE PyObject*
#define Ci_STACK_NULL NULL
#define Ci_STACK_STEAL(VAL) VAL
#define Ci_STACK_CLEAR(VAL) Py_CLEAR(VAL)
#define Ci_STACK_XSETREF(DST, VAL) Py_XSETREF(DST, VAL);
#define Ci_STACK_NEWREF(VAL) Py_NewRef(VAL)
#define Ci_STACK_CLOSE(VAL) Py_DECREF(VAL)
#define Ci_STACK_XCLOSE(VAL) Py_XDECREF(VAL)

#else

#define Ci_STACK_TYPE _PyStackRef
#define Ci_STACK_NULL PyStackRef_NULL
#define Ci_STACK_STEAL(VAL)                                      \
  [&]() {                                                        \
    auto _val = (VAL);                                           \
    return _val == nullptr ? PyStackRef_NULL                     \
                           : PyStackRef_FromPyObjectSteal(_val); \
  }()
#define Ci_STACK_CLEAR(VAL) PyStackRef_CLEAR(VAL)
#define Ci_STACK_XSETREF(DST, VAL)                     \
  do {                                                 \
    _PyStackRef* _tmp_dst_ptr = &(DST);                \
    _PyStackRef _tmp_old_dst = (*_tmp_dst_ptr);        \
    *_tmp_dst_ptr = PyStackRef_FromPyObjectSteal(VAL); \
    PyStackRef_XCLOSE(_tmp_old_dst);                   \
  } while (0)
#define Ci_STACK_NEWREF(VAL) _PyStackRef_FromPyObjectNew(VAL)
#define Ci_STACK_CLOSE(VAL) PyStackRef_CLOSE(VAL)
#define Ci_STACK_XCLOSE(VAL) PyStackRef_XCLOSE(VAL)

#endif

#if PY_VERSION_HEX >= 0x030E0000
#define _PyObject_Call(tstate, callable, args, kwargs) \
  PyObject_Call(callable, args, kwargs)
#endif

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_opcode_utils.h"
#else
#define MAKE_FUNCTION_DEFAULTS 0x01
#define MAKE_FUNCTION_KWDEFAULTS 0x02
#define MAKE_FUNCTION_ANNOTATIONS 0x04
#define MAKE_FUNCTION_CLOSURE 0x08
#endif

#if PY_VERSION_HEX < 0x030E0000
#define Ci_Type_HasValidVersionTag(type) \
  PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)
#else
#define Ci_Type_HasValidVersionTag(type) ((type)->tp_version_tag != 0)
#endif

#if PY_VERSION_HEX < 0x030D0000
static inline int PyWeakref_GetRef(PyObject* ref, PyObject** pobj) {
  if (ref == NULL || !PyWeakref_Check(ref)) {
    return -1;
  }
  *pobj = PyWeakref_GET_OBJECT(ref);
  if (*pobj == Py_None) {
    *pobj = NULL;
    return 0;
  }
  Py_INCREF(*pobj);
  return 1;
}
#endif
