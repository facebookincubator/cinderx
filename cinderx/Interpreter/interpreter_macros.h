// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/code_extra.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
struct CiInterpreterLoopState {
  uintptr_t bits;
};
#define CI_INLINE inline
static_assert(
    alignof(CodeExtra) >= 2,
    "CodeExtra pointers must leave the low bit available for interpreter "
    "state");
#else
typedef struct {
  uintptr_t bits;
} CiInterpreterLoopState;
#define CI_INLINE static inline
_Static_assert(
    _Alignof(CodeExtra) >= 2,
    "CodeExtra pointers must leave the low bit available for interpreter "
    "state");
#endif

#define CI_ADAPTIVE_ENABLED_TAG ((uintptr_t)1)

CI_INLINE CiInterpreterLoopState
ci_pack_interpreter_loop_state(CodeExtra* extra, bool adaptive_enabled) {
  // CodeExtra is pointer-aligned, so its low bit can store the adaptive state.
  assert(((uintptr_t)extra & CI_ADAPTIVE_ENABLED_TAG) == 0);
  return (CiInterpreterLoopState){
      .bits =
          (uintptr_t)extra | (adaptive_enabled ? CI_ADAPTIVE_ENABLED_TAG : 0),
  };
}

CI_INLINE bool ci_interpreter_loop_state_adaptive_enabled(
    CiInterpreterLoopState state) {
  return (state.bits & CI_ADAPTIVE_ENABLED_TAG) != 0;
}

CI_INLINE CodeExtra* ci_interpreter_loop_state_extra(
    CiInterpreterLoopState state) {
  return (CodeExtra*)(state.bits & ~CI_ADAPTIVE_ENABLED_TAG);
}

#define CI_ADAPTIVE_ENABLED() \
  ci_interpreter_loop_state_adaptive_enabled(ci_state)
#define CI_CODE_EXTRA() ci_interpreter_loop_state_extra(ci_state)

#undef CI_INLINE
