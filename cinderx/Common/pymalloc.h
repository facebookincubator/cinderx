// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace cinderx {

// Call once, before the first fork: allocate never-freed ballast until the
// main interpreter's used and previously used pymalloc pools have no free
// space, so a forked child allocates from fresh pages instead of copying the
// parent's.  Each call pins everything freed since the previous one, so calling
// it before every fork grows the parent.  Returns (blocks, pools) allocated as
// a tuple; (0, 0) without pymalloc.
PyObject* fillPymallocPools();

} // namespace cinderx
