// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/module_c_state.h"

// CiExc_StaticTypeError is stored in ModuleState and accessed via a C
// accessor function. The macro preserves source compatibility with all
// existing usage sites.
#define CiExc_StaticTypeError Ci_GetStaticTypeError()
