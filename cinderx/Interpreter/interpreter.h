// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

// Exporting Ci_PyFunction_Vectorcall.
#include "cinderx/module_c_state.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The CinderX frame evaluator function (interpreter loop).
 */
#if PY_VERSION_HEX < 0x030C0000
PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState* tstate, PyFrameObject* f, int throwflag);
#else
PyObject* _Py_HOT_FUNCTION Ci_EvalFrame(
    PyThreadState* tstate,
    struct _PyInterpreterFrame* f,
    int throwflag);
#endif

/*
 * General vectorcall entry point to a function compiled by the Static Python
 * compiler.  The function will be executed in the interpreter.
 */
PyObject* Ci_StaticFunction_Vectorcall(
    PyObject* func,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Optimized form of Ci_StaticFunction_Vectorcall, where all arguments are
 * guaranteed to have the correct type and do not use `kwnames`.
 */
PyObject* Ci_PyFunction_CallStatic(
    PyFunctionObject* func,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Get the appropriate entry point that will execute a function object in the
 * interpreter.
 *
 * This is a different function for Static Python functions versus "normal"
 * Python functions.
 */
vectorcallfunc getInterpretedVectorcall(const PyFunctionObject* func);

/*
 * Install the CinderX frame evaluator function into the runtime.
 */
int Ci_InitFrameEvalFunc();

/*
 * Remove the CinderX frame evaluator function from the runtime.
 */
void Ci_FiniFrameEvalFunc();

void Ci_InitOpcodes();

extern bool Ci_DelayAdaptiveCode;
extern uint64_t Ci_AdaptiveThreshold;

#ifdef __cplusplus
}
#endif
