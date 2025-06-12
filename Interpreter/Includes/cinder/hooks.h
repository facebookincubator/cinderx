// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"

/* Hooks needed by CinderX that have not been added to upstream. */

PyAPI_DATA(_PyFrameEvalFunction) Ci_hook_EvalFrame;

#ifdef __cplusplus
} // extern "C"
#endif
