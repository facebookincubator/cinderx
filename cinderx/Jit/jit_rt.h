// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030C0000
#include <utility>
#endif

#include "cinderx/Common/util.h"
#include "cinderx/StaticPython/typed-args-info.h"

namespace jit {
class CodeRuntime;
struct GenDataFooter;
struct JitGenObject;
} // namespace jit

// static->static call convention for primitive returns is to return error flag
// in rdx (null means error occurred); for C helpers that need to implement this
// convention, returning this struct will fill the right registers
typedef struct {
  void* rax;
  void* rdx;
} JITRT_StaticCallReturn;

typedef struct {
  double xmm0;
  double xmm1;
} JITRT_StaticCallFPReturn;

#if PY_VERSION_HEX < 0x030C0000
/*
 * Allocate a new PyFrameObject and link it into the current thread's
 * call stack.
 *
 * Returns the thread state that the freshly allocated frame was linked to
 * (accessible via ->frame) on success or NULL on error.
 */
PyThreadState* JITRT_AllocateAndLinkFrame(
    PyCodeObject* code,
    PyObject* builtins,
    PyObject* globals);
#else

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Debug(
    PyFunctionObject* func,
    PyCodeObject* jit_code_object);

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Release(
    PyFunctionObject* func);

std::pair<PyThreadState*, jit::GenDataFooter*>
JITRT_AllocateAndLinkGenAndInterpreterFrame(
    PyFunctionObject* func,
    uint64_t spill_words,
    jit::CodeRuntime* code_rt,
    GenResumeFunc resume_entry,
    uint64_t original_rbp);

void JITRT_InitFrameCellVars(
    PyFunctionObject* func,
    int nvars,
    PyThreadState* tstate);

std::pair<jit::JitGenObject*, jit::GenDataFooter*>
JITRT_UnlinkGenFrameAndReturnGenDataFooter(PyThreadState* tstate);

#endif

/*
 * Helper function to decref a frame.
 *
 * Used by JITRT_UnlinkFrame, and designed to only be used separately if
 * something else has already unlinked the frame.
 */
void JITRT_DecrefFrame(PyFrameObject* frame);

/*
 * Helper function to unlink a frame.
 *
 * Designed to be used in tandem with JITRT_AllocateAndLinkFrame. This checks
 * if the frame has escaped (> 1 refcount) and tracks it if so.
 */
void JITRT_UnlinkFrame(PyThreadState* tstate);

/*
 * Handles a call that includes kw arguments or excess tuple arguments
 */
PyObject* JITRT_CallWithKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

JITRT_StaticCallReturn JITRT_CallWithIncorrectArgcount(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount);

JITRT_StaticCallFPReturn JITRT_CallWithIncorrectArgcountFPReturn(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount);

/* Helper function to report an error when the arguments aren't correct for
 * a static function call.  Dispatches to the eval loop to let the normal
 * argument checking prologue run and then report the error */
PyObject* JITRT_ReportStaticArgTypecheckErrors(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/* Variation of JITRT_ReportStaticArgTypecheckErrors but also sets the primitive
   return value error in addition to returning the normal NULL error indicator
 */
JITRT_StaticCallReturn JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/* Variation of JITRT_ReportStaticArgTypecheckErrors but also sets the double
   return value error in addition to returning the normal NULL error indicator
 */
JITRT_StaticCallFPReturn JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Mimics the behavior of Cix_PyDict_LoadGlobal except that it raises an error
 * when the name does not exist.
 */
PyObject*
JITRT_LoadGlobal(PyObject* globals, PyObject* builtins, PyObject* name);

/*
 * Load a global value given a Python thread state.
 */
PyObject* JITRT_LoadGlobalFromThreadState(
    PyThreadState* tstate,
    PyObject* name);

/*
 * Load the globals dict from a Python thread state.
 */
PyObject* JITRT_LoadGlobalsDict(PyThreadState* tstate);

/*
 * Helper to perform a Python call with dynamically determined arguments.
 *
 * pargs will be a possibly empty tuple of positional arguments, kwargs will be
 * null or a dictionary of keyword arguments.
 */
PyObject*
JITRT_CallFunctionEx(PyObject* func, PyObject* pargs, PyObject* kwargs);

/*
 * As JITRT_CallFunctionEx but eagerly starts coroutines.
 */
PyObject*
JITRT_CallFunctionExAwaited(PyObject* func, PyObject* pargs, PyObject* kwargs);

/*
 * Perform a function or method call.
 *
 * If it's a method call, then `args[0]` will be the receiver of the method
 * lookup (e.g. `self`). The rest of `args` will be the positional and keyword
 * arguments to the call.
 *
 * If it's a function call, then `callable` will be Py_None and the actual
 * callable will be stored in `args[0]`.  The rest of `args` is then the same as
 * the method case.
 *
 * Note: Technically for the function call case, `callable` should be NULL and
 * not Py_None, but we use NULL return values in HIR to determine where to
 * deopt.
 */
PyObject* JITRT_Call(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Performs a function call with a vectorcall. Will check and handle any
 * eval breaker events after the call.
 */
PyObject* JITRT_Vectorcall(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Perform a method lookup on an object.
 */
LoadMethodResult JITRT_GetMethod(PyObject* obj, PyObject* name);

/*
 * Perform an attribute lookup in a super class
 *
 * This is used to avoid bound method creation for attribute lookups that
 * correspond to method calls (e.g. `self.foo()`).
 */
LoadMethodResult JITRT_GetMethodFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call);

/*
 * Perform an attribute lookup in a super class
 */
PyObject* JITRT_GetAttrFromSuper(
    PyObject* super_globals,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call);

/*
 * Mimics the behavior of the UNARY_NOT opcode.
 *
 * Checks if value is truthy, and returns Py_False if it is, or Py_True if
 * it's not.  Returns NULL if the object doesn't support truthyness.
 */
PyObject* JITRT_UnaryNot(PyObject* value);

/*
 * Invokes a function stored within the method table for the object.
 * The method table lives off tp_cache in the type object
 */
PyObject* JITRT_InvokeMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject* kwnames);
/*
 * Invokes a function stored within the method table for the object.
 * The method table lives off tp_cache of self.
 */
PyObject* JITRT_InvokeClassMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject* kwnames);

/*
 * Loads an indirect function, optionally loading it from the descriptor
 * if the indirect cache fails.
 */
PyObject* JITRT_LoadFunctionIndirect(PyObject** func, PyObject* descr);

/*
 * Performs a type check on an object, raising an error if the object is
 * not an instance of the specified type.  The type check is a real type
 * check which doesn't support dynamic behaviors against the type or
 * proxy behaviors against obj.__class__
 */
PyObject* JITRT_Cast(PyObject* obj, PyTypeObject* type);

/*
 * JITRT_Cast when target type is float. This case requires extra work
 * because Python typing pretends int is a subtype of float, so CAST
 * needs to coerce int to float.
 */
PyObject* JITRT_CastToFloat(PyObject* obj);

/*
 * JITRT_CastToFloat but with None allowed.
 */
PyObject* JITRT_CastToFloatOptional(PyObject* obj);

/*
 * Performs a type check on an object, raising an error if the object is
 * not an instance of the specified type or None.  The type check is a
 * real type check which doesn't support dynamic behaviors against the
 * type or proxy behaviors against obj.__class__.
 */
PyObject* JITRT_CastOptional(PyObject* obj, PyTypeObject* type);
/* Performs a type check on obj, but does not allow passing a subclass of type.
 */
PyObject* JITRT_CastExact(PyObject* obj, PyTypeObject* type);
PyObject* JITRT_CastOptionalExact(PyObject* obj, PyTypeObject* type);

/* Helper methods to implement left shift, which wants its operand in cl */
int64_t JITRT_ShiftLeft64(int64_t x, int64_t y);
int32_t JITRT_ShiftLeft32(int32_t x, int32_t y);

/* Helper methods to implement right shift, which wants its operand in cl */
int64_t JITRT_ShiftRight64(int64_t x, int64_t y);
int32_t JITRT_ShiftRight32(int32_t x, int32_t y);

/* Helper methods to implement unsigned right shift, which wants its operand in
 * cl
 */
uint64_t JITRT_ShiftRightUnsigned64(uint64_t x, uint64_t y);
uint32_t JITRT_ShiftRightUnsigned32(uint32_t x, uint32_t y);

/* Helper methods to implement signed modulus */
int64_t JITRT_Mod64(int64_t x, int64_t y);
int32_t JITRT_Mod32(int32_t x, int32_t y);

/* Helper methods to implement unsigned modulus */
uint64_t JITRT_ModUnsigned64(uint64_t x, uint64_t y);
uint32_t JITRT_ModUnsigned32(uint32_t x, uint32_t y);

PyObject* JITRT_BoxI32(int32_t i);
PyObject* JITRT_BoxU32(uint32_t i);
PyObject* JITRT_BoxBool(uint32_t i);
PyObject* JITRT_BoxI64(int64_t i);
PyObject* JITRT_BoxU64(uint64_t i);
PyObject* JITRT_BoxDouble(double_t d);

double JITRT_PowerDouble(double x, double y);
double JITRT_Power32(int32_t x, int32_t y);
double JITRT_PowerUnsigned32(uint32_t x, uint32_t y);
double JITRT_Power64(int64_t x, int64_t y);
double JITRT_PowerUnsigned64(uint64_t x, uint64_t y);

/* Array set helpers */
void JITRT_SetI8_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetU8_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetI16_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetU16_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetI32_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetU32_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetI64_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetU64_InArray(char* arr, uint64_t val, int64_t idx);
void JITRT_SetObj_InArray(char* arr, uint64_t val, int64_t idx);

uint64_t JITRT_UnboxU64(PyObject* obj);
uint32_t JITRT_UnboxU32(PyObject* obj);
uint16_t JITRT_UnboxU16(PyObject* obj);
uint8_t JITRT_UnboxU8(PyObject* obj);
int64_t JITRT_UnboxI64(PyObject* obj);
int32_t JITRT_UnboxI32(PyObject* obj);
int16_t JITRT_UnboxI16(PyObject* obj);
int8_t JITRT_UnboxI8(PyObject* obj);

/*
 * Calls __builtins__.__import__(), with a fast-path if this hasn't been
 * overridden.
 *
 * This is a near verbatim copy of import_name() from ceval.c with minor
 * tweaks. We copy rather than expose to avoid making changes to ceval.c.
 */
PyObject* JITRT_ImportName(
    PyThreadState* tstate,
    PyObject* name,
    PyObject* fromlist,
    PyObject* level);

/*
 * Wrapper around _Py_DoRaise() which handles the case where we re-raise but no
 * active exception is set.
 */
void JITRT_DoRaise(PyThreadState* tstate, PyObject* exc, PyObject* cause);

/*
 * Formats a f-string value
 */
PyObject* JITRT_FormatValue(
    PyThreadState* tstate,
    PyObject* fmt_spec,
    PyObject* value,
    int conversion);
/*
 * Concatenate strings from args
 */
PyObject* JITRT_BuildString(
    void* /*unused*/,
    PyObject** args,
    size_t nargsf,
    void* /*unused*/);

#if PY_VERSION_HEX < 0x030C0000
/*
 * Create generator instance for use during InitialYield in a JIT generator.
 * There is a variant for each of the different types of generator: iterators,
 * coroutines, and async generators.
 */
PyObject* JITRT_MakeGenObject(
    PyThreadState* tstate,
    GenResumeFunc resume_entry,
    size_t spill_words,
    jit::CodeRuntime* code_rt,
    PyCodeObject* code);

PyObject* JITRT_MakeGenObjectAsyncGen(
    PyThreadState* tstate,
    GenResumeFunc resume_entry,
    size_t spill_words,
    jit::CodeRuntime* code_rt,
    PyCodeObject* code);

PyObject* JITRT_MakeGenObjectCoro(
    PyThreadState* tstate,
    GenResumeFunc resume_entry,
    size_t spill_words,
    jit::CodeRuntime* code_rt,
    PyCodeObject* code);
#endif

// Set the awaiter of the given awaitable to be the coroutine at the top of
// `ts`.
void JITRT_SetCurrentAwaiter(PyObject* awaitable, PyThreadState* ts);

// Mostly the same implementation as YIELD_FROM in ceval.c with slight tweaks to
// make it stand alone. The argument 'v' is stolen.
//
// The arguments 'gen', 'v', 'finish_yield_from' must match positions with JIT
// resume entry function (GenResumeFunc) so registers with their values pass
// straight through.
struct JITRT_GenSendRes {
  PyObject* retval;
  uint64_t done;
};
JITRT_GenSendRes JITRT_GenSend(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from
#if PY_VERSION_HEX >= 0x030C0000
    ,
    _PyInterpreterFrame* frame
#endif
);

// Used for the `YIELD_FROM` that appears in the bytecode of the header for
// an `async for` loop.
//
// This is identical to JITRT_GenSend with the addition that it detects when
// PyExc_StopAsyncIteration has been raised. In such cases the function clears
// the error and returns a sentinel value indicating that iteration has
// finished.
JITRT_GenSendRes JITRT_GenSendHandleStopAsyncIteration(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from
#if PY_VERSION_HEX >= 0x030C0000
    ,
    _PyInterpreterFrame* frame
#endif
);

/* Unpack a sequence as in unpack_iterable(), and save the
 * results in a tuple.
 */
PyObject* JITRT_UnpackExToTuple(
    PyThreadState* tstate,
    PyObject* iterable,
    int before,
    int after);

/* When compiling a fully-typed JIT static -> static call we sometimes
 * optimistically assume the target will be JIT compiled too. If the target
 * fails to compile we point the call to this function which converts the static
 * arguments into a form suitable for a regular Python vector call. Much of the
 * work in this function would have to be done anyway if we were initially
 * making a JIT static -> non-JIT static function anyway, so there is not too
 * much overhead.
 */
JITRT_StaticCallReturn JITRT_FailedDeferredCompileShim(
    PyFunctionObject* func,
    PyObject** args);

JITRT_StaticCallReturn JITRT_CallStaticallyWithPrimitiveSignature(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info);

JITRT_StaticCallFPReturn JITRT_CallStaticallyWithPrimitiveSignatureFP(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info);

/* Compares if one unicode object is equal to another object.
 * At least one of the objects has to be exactly a unicode
 * object.
 */
int JITRT_UnicodeEquals(PyObject* s1, PyObject* s2, int equals);

/* Return Py_True if needle is in haystack else return Py_False. Return nullptr
 * with exception raised on error. */
PyObject* JITRT_SequenceContains(PyObject* haystack, PyObject* needle);

/* Return Py_True if needle is not in haystack else return Py_False. Return
 * nullptr with exception raised on error. */
PyObject* JITRT_SequenceNotContains(PyObject* haystack, PyObject* needle);

/* Inverse form of PySequence_Contains for "not in"
 */
int JITRT_NotContainsBool(PyObject* w, PyObject* v);

/* Perform a rich comparison with integer result.  This wraps
   PyObject_RichCompare(), returning -1 for error, 0 for false, 1 for true.
   Unlike PyObject_RichCompareBool this doesn't perform an object equality
   check, which is incompatible w/ float comparisons. */

int JITRT_RichCompareBool(PyObject* v, PyObject* w, int op);

/* perform a batch decref to the objects in args */
void JITRT_BatchDecref(PyObject** args, int nargs);

/* Check that `i` is within the bounds of `seq`.
 *
 * A negative value of `i` is an index relative to the end of the
 * sequence (e.g. -1 refers to the last element in the sequence).
 *
 * Returns 0-based index that `i` refers to on success.
 * Returns -1 and raises IndexError on error.
 **/
Py_ssize_t JITRT_CheckSequenceBounds(PyObject* seq, Py_ssize_t i);

/* Call obj.__len__(). Return LongExact on success or NULL with an exception
 * set if there was an error. */
PyObject* JITRT_GetLength(PyObject* obj);

/* Call match_keys() in ceval.c
 * NOTE: This function is here as a wrapper around the private match_keys
 * function and should be removed when match_keys becomes public.
 */
PyObject*
JITRT_MatchKeys(PyThreadState* tstate, PyObject* subject, PyObject* keys);

/* Used by DICT_UPDATE and DICT_MERGE implementations. */
int JITRT_DictUpdate(PyThreadState* tstate, PyObject* dict, PyObject* update);
int JITRT_DictMerge(
    PyThreadState* tstate,
    PyObject* dict,
    PyObject* update,
    PyObject* func);

/* Returns nullptr on error and an exact dict otherwise. Used by
 * COPY_DICT_WITHOUT_KEYS implementation. */
PyObject* JITRT_CopyDictWithoutKeys(PyObject* subject, PyObject* keys);

/* Load a name from a Python thread's code object.
 */
PyObject* JITRT_LoadName(PyThreadState* tstate, int name_idx);

/* Reimplements the format_awaitable_error() function from the CPython
 * interpreter loop. */
void JITRT_FormatAwaitableError(
    PyThreadState* tstate,
    PyTypeObject* type,
    bool is_aenter);

void JITRT_IncRefTotal();
void JITRT_DecRefTotal();

#if PY_VERSION_HEX >= 0x030C0000
PyObject* JITRT_LookupAttrSpecial(
    PyObject* obj,
    PyObject* attr,
    const char* failure_fmt_str);

#endif
