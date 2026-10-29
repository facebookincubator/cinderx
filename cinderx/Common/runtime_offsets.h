// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

// Runtime-correct offsets for CPython interpreter and thread-state fields.
//
// CinderX is frequently loaded into a Python runtime built from different
// headers than CinderX itself (platform runtimes, OSS patch releases), and
// CPython does not guarantee struct-layout stability across patch versions.
// For example, the CPython 3.14.9 import changed the size of
// `_mimalloc_interp_state` (320 -> 8 bytes) and `_mimalloc_thread_state`
// (+3608 bytes), shifting every field after them in `PyInterpreterState`
// and `_PyThreadStateImpl`.
//
// Direct field accesses compiled against one layout read garbage when the
// loaded runtime uses another (observed as a segfault in `Ci_EvalFrame`
// from a wild `tstate->tlbc_index`). Code in CinderX that touches the
// affected regions must therefore bias its accesses by deltas derived at
// runtime from `_PyRuntime.debug_offsets`:
//
//   Region A (delta 0): everything up to and including `interp->gc`, plus
//     all of `_PyRuntimeState` ahead of `_main_interpreter`. Access directly.
//   Region B (Ci_InterpPreDelta): `interp->[sysdict..qsbr]`, anchored on the
//     `code_object_generation` debug offset. In GIL builds this delta also
//     covers every field after `interp->gc` (there is no mimalloc block).
//   Region C (Ci_InterpPostDelta): `interp->[brc.._initial_thread)` in
//     free-threaded builds, anchored on the `tlbc_generation` debug offset.
//   Region D (Ci_ThreadPostDelta): `_PyThreadStateImpl->[freelists..end]` in
//     free-threaded builds. No debug offset anchors this region, so the
//     direction comes from the signs of the (pre, post, sizediff) interp
//     signature -- absolute values vary by platform -- and the magnitude
//     from CI_THREAD_MIMALLOC_DELTA. The delta is verified once against
//     brc.tid when the module state is created. Unknown signatures report
//     through Ci_ThreadLayoutKnown() so callers can degrade gracefully
//     instead of reading memory at a wrong offset.
//   Region E (Ci_InterpMetaDelta): `interp` fields after `_initial_thread`
//     (the META_PYTHON lazy-imports state). This is region C plus region D.
//
// Prefer the CiInterpPre()/CiInterpPost()/CiInterpMeta()/CiTstatePost()
// biased-pointer helpers over open-coding the arithmetic.

#include "cinderx/python.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#include "internal/pycore_runtime.h"
// NOTE: do not include pycore_pystate.h here. CiTstatePost() only casts to
// _PyThreadStateImpl*, which is legal on an incomplete type, and pulling in
// thread-state declarations early breaks the _Ci symbol renames in
// cinderx/UpstreamBorrow/borrowed.h (see the comment there).
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PY_VERSION_HEX >= 0x030E0000

// Magnitude of the _mimalloc_thread_state growth between CPython 3.14.8
// (mimalloc 2.1.2) and 3.14.9+ (mimalloc 2.5.2). Measured on Linux x86-64
// and runtime-verified on macOS ARM64. Windows (LLP64) differs by 32: two
// `long` fields added to mi_heap_t are 4 bytes there instead of 8 (4
// heaps x 2 fields x 4 bytes), measured by compiling the mimalloc headers
// for x86_64-pc-windows-msvc. The loaded runtime always shares the
// build's OS, so selecting by build OS is sound; the resulting delta is
// verified against brc.tid when the module state is created.
#ifdef _WIN32
#define CI_THREAD_MIMALLOC_DELTA 3576
#else
#define CI_THREAD_MIMALLOC_DELTA 3608
#endif

// Pure computation over a (pre, post, sizediff) debug-offset signature.
// `pre` is the runtime-minus-compiled delta of `code_object_generation`,
// `post` the same for `tlbc_generation`, and `sizediff` the
// runtime-minus-compiled delta of `sizeof(PyInterpreterState)`. `is_py314`
// gates the 3.14-only direction check. Returns nonzero with `*out_delta`
// set when the signature matches a known layout (or identical layouts);
// returns zero when the thread-state layout cannot be determined.
static inline int Ci_ComputeThreadDelta(
    int64_t pre,
    int64_t post,
    int64_t sizediff,
    int is_py314,
    int64_t* out_delta) {
  // Identical layouts: every delta is zero. The size check catches
  // thread-state-only drift that neither anchor can see.
  if (pre == 0 && post == 0) {
    if (sizediff != 0) {
      return 0;
    }
    *out_delta = 0;
    return 1;
  }
  if (is_py314) {
    // CPython 3.14.8 (mimalloc 2.1.2) vs 3.14.9+ (mimalloc 2.5.2).
    // Absolute signature values vary by platform (struct padding, mutex
    // sizes): Linux x86-64 measures (pre=0, post=+320, sizediff=-3256)
    // while macOS ARM64 measures (0, +336, -3240). Only the direction is
    // portable, so match on signs: a positive post-vs-pre shift with a
    // negative size change means the runtime predates the build (its
    // thread mimalloc block is smaller, so post-block runtime offsets are
    // smaller), and vice versa.
    int64_t rel = post - pre;
    if (rel > 0 && sizediff < 0) {
      *out_delta = -CI_THREAD_MIMALLOC_DELTA;
      return 1;
    }
    if (rel < 0 && sizediff > 0) {
      *out_delta = CI_THREAD_MIMALLOC_DELTA;
      return 1;
    }
  }
  return 0;
}

// All deltas are pure functions of _PyRuntime.debug_offsets and the
// compiled layout, both process-invariant. The single instance lives in
// cinderx::ModuleState, computed once in setModuleState() before any
// biased access can run; helpers below load it with no init check.
struct Ci_RuntimeDeltas {
  int64_t interp_pre;
  int64_t interp_post;
  int64_t thread;
  int thread_known;
};

// Returns the module state's cached deltas (defined in module_c_state.cpp).
// The state is set during _cinderx module exec, which precedes every biased
// access; callers do not null-check.
const struct Ci_RuntimeDeltas* Ci_GetRuntimeDeltas(void);

// Reads the current (pre, post, sizediff) thread-signature triple. Used by
// the one-time init and, on cold fatal paths, to report the offending
// signature; the values are deterministic, so recomputing is exact.
static inline void
Ci_ThreadSignature(int64_t* pre, int64_t* post, int64_t* sizediff) {
  const _Py_DebugOffsets* offsets = &_PyRuntime.debug_offsets;
  *pre = (int64_t)offsets->interpreter_state.code_object_generation -
      (int64_t)offsetof(PyInterpreterState, _code_object_generation);
#ifdef Py_GIL_DISABLED
  *post = (int64_t)offsets->interpreter_state.tlbc_generation -
      (int64_t)offsetof(PyInterpreterState, tlbc_indices.tlbc_generation);
#else
  // GIL builds have no tlbc_indices; post matches pre, mirroring
  // Ci_InterpPostDelta().
  *post = *pre;
#endif
  *sizediff = (int64_t)offsets->interpreter_state.size -
      (int64_t)sizeof(PyInterpreterState);
}

// Delta to add to compiled offsets of region B fields (and, in GIL builds,
// every field after interp->gc).
static inline int64_t Ci_InterpPreDelta(void) {
  return Ci_GetRuntimeDeltas()->interp_pre;
}

#ifdef Py_GIL_DISABLED
// Delta to add to compiled offsets of region C fields.
static inline int64_t Ci_InterpPostDelta(void) {
  return Ci_GetRuntimeDeltas()->interp_post;
}
#else
static inline int64_t Ci_InterpPostDelta(void) {
  return Ci_InterpPreDelta();
}
#endif

#ifdef Py_GIL_DISABLED
// Whether the region D (thread-state) layout is known. False means CinderX
// must not install its eval loop or JIT: there is no correct offset for
// fields such as tlbc_index. A reported-known delta was verified against
// brc.tid when the module state was created.
static inline int Ci_ThreadLayoutKnown(void) {
  return Ci_GetRuntimeDeltas()->thread_known;
}

// Delta to add to compiled offsets of region D fields. Aborts when the
// layout is unknown; callers that can degrade must check
// Ci_ThreadLayoutKnown() first instead of reaching here.
static inline int64_t Ci_ThreadPostDelta(void) {
  const struct Ci_RuntimeDeltas* deltas = Ci_GetRuntimeDeltas();
  if (!deltas->thread_known) {
    int64_t pre = 0, post = 0, sizediff = 0;
    Ci_ThreadSignature(&pre, &post, &sizediff);
    char msg[256];
    snprintf(
        msg,
        sizeof(msg),
        "CinderX: unknown CPython thread-state layout "
        "(pre=%lld post=%lld sizediff=%lld); refusing to read fields at a "
        "guessed offset",
        (long long)pre,
        (long long)post,
        (long long)sizediff);
    Py_FatalError(msg);
  }
  return deltas->thread;
}

// Biased base pointer for a known thread state: loads the cached delta
// (verified against brc.tid when the module state was created) and adjusts
// the base. Aborts when the layout is unknown.
static inline char* Ci_TstatePostFor(void* tstate) {
  return (char*)tstate + Ci_ThreadPostDelta();
}

// Delta to add to compiled offsets of region E (META) fields. The region D
// component reuses the Region D delta value, verified when the module
// state was created; E itself performs no check.
static inline int64_t Ci_InterpMetaDelta(void) {
  return Ci_InterpPostDelta() + Ci_ThreadPostDelta();
}
#endif // Py_GIL_DISABLED

// Biased interpreters: use the returned pointer only for fields in the
// matching region.
#define CiInterpPre(interp) \
  ((PyInterpreterState*)((char*)(interp) + Ci_InterpPreDelta()))
#define CiInterpPost(interp) \
  ((PyInterpreterState*)((char*)(interp) + Ci_InterpPostDelta()))

#ifdef Py_GIL_DISABLED
// Biased thread state: use the returned pointer only for region D fields
// (freelists and later). Aborts when the layout is unknown. Evaluates
// tstate exactly once.
#define CiTstatePost(tstate) ((_PyThreadStateImpl*)Ci_TstatePostFor((tstate)))
#define CiInterpMeta(interp) \
  ((PyInterpreterState*)((char*)(interp) + Ci_InterpMetaDelta()))
#else
// GIL builds have no region D; region E matches region B.
#define CiInterpMeta(interp) CiInterpPre(interp)
#endif

// Runtime-correct address of a runtime's embedded main interpreter.
// _main_interpreter is the last field of _PyRuntimeState, so its offset is
// the runtime size minus the interpreter size. Do not use
// &(runtime)->_main_interpreter directly: the embedded interpreter itself
// moves when the fields ahead of it change size.
static inline PyInterpreterState* Ci_MainInterpreter(_PyRuntimeState* runtime) {
  const _Py_DebugOffsets* offsets = &runtime->debug_offsets;
  return (PyInterpreterState*)((char*)runtime +
                               ((int64_t)offsets->runtime_state.size -
                                (int64_t)offsets->interpreter_state.size));
}

#else // PY_VERSION_HEX < 0x030E0000

// Pre-3.14 runtimes predate the debug-offset table and have no known layout
// skew; keep direct accesses.
#define CiInterpPre(interp) (interp)
#define CiInterpPost(interp) (interp)
#define CiInterpMeta(interp) (interp)
#define CiTstatePost(tstate) (tstate)
#define Ci_MainInterpreter(runtime) (&(runtime)->_main_interpreter)

static inline int Ci_ThreadLayoutKnown(void) {
  return 1;
}

#endif // PY_VERSION_HEX >= 0x030E0000

#ifdef __cplusplus
} // extern "C"
#endif
