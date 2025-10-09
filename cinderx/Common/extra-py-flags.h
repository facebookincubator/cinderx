// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * A place to put extra CinderX-only flags used on existing Python objects.
 */

#pragma once

#include "cinderx/python.h"

// Additional PyCodeObject flags (see Include/code.h)
#if PY_VERSION_HEX >= 0x030E0000
#define CI_CO_STATICALLY_COMPILED 0x4000
#else
#define CI_CO_STATICALLY_COMPILED 0x4000000
#endif

#if PY_VERSION_HEX >= 0x030E0000

#define Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED (1UL << 21)

#elif PY_VERSION_HEX >= 0x030C0000

// Lowest bit is unused
#define Ci_Py_TPFLAGS_IS_STATICALLY_DEFINED (1UL << 2)

#endif
