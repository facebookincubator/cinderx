// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace cinderx::jit {

/*
 * Replace PyFunction_Type's GC slots with versions that also account for the
 * logical reference a JIT-compiled function holds on its CompiledFunction.
 *
 * PyFunction_Type is process-global and shared across sub-interpreters, so
 * this follows the same save-and-restore pattern as init_jit_genobject_type().
 */
void initJitFunctionSlots();

// Put PyFunction_Type's original GC slots back.
void shutdownJitFunctionSlots();

} // namespace cinderx::jit
