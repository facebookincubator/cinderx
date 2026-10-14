// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/pymalloc.h"

#include "cinderx/Common/ref.h"

#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && \
    defined(WITH_PYMALLOC) && !defined(Py_GIL_DISABLED)
#define CINDER_PYMALLOC_FILL
#include "internal/pycore_interp.h"
#include "internal/pycore_obmalloc.h"
#endif

namespace cinderx {

#ifdef CINDER_PYMALLOC_FILL

namespace {

bool checkMainInterpreter() {
  if (PyInterpreterState_Get() != PyInterpreterState_Main()) {
    PyErr_SetString(
        PyExc_RuntimeError, "pymalloc fill needs the main interpreter");
    return false;
  }
  return true;
}

_obmalloc_state* mainObmalloc() {
  return &PyInterpreterState_Main()->obmalloc;
}

// Never-freed ballast, chained through each block's first word (which pymalloc
// has already written), so holding it costs nothing extra.
void* g_ballast = nullptr;

struct FillStats {
  Py_ssize_t blocks{0};
  Py_ssize_t pools{0};
};

// The pool that the next allocation of size class `szidx` will come from, or
// nullptr if it would come from a never-used pool or a new arena.  `fresh` is
// set if the pool is free.  Never-used pools are left alone: the parent hasn't
// touched their pages, and filling them would make later allocations open new
// arenas for the next call to fill.
poolp nextPool(_obmalloc_state* state, unsigned int szidx, bool& fresh) {
  poolp pool = state->pools.used[szidx + szidx];
  fresh = pool == pool->nextpool;
  if (!fresh) {
    return pool;
  }
  arena_object* arena = state->mgmt.usable_arenas;
  return arena != nullptr ? arena->freepools : nullptr;
}

// Allocate ballast of size class `szidx` while used or previously used pools
// can serve it, taking free pools only if `carve`.  Returns false if
// PyObject_Malloc isn't served directly by pymalloc (e.g. debug hooks are
// installed).
bool fillSizeClass(
    _obmalloc_state* state,
    unsigned int szidx,
    bool carve,
    FillStats& stats) {
  for (;;) {
    bool fresh = false;
    poolp pool = nextPool(state, szidx, fresh);
    if (pool == nullptr || (fresh && !carve)) {
      return true;
    }
    void* block = PyObject_Malloc(INDEX2SIZE(szidx));
    if (block == nullptr || POOL_ADDR(block) != pool || pool->szidx != szidx) {
      PyObject_Free(block);
      return false;
    }
    *static_cast<void**>(block) = g_ballast;
    g_ballast = block;
    stats.blocks++;
    stats.pools += fresh;
  }
}

void fill(_obmalloc_state* state, FillStats& stats) {
  for (unsigned int szidx = 0; szidx < NB_SMALL_SIZE_CLASSES; szidx++) {
    if (!fillSizeClass(state, szidx, false, stats)) {
      return;
    }
  }
  // The largest size class fills a free pool in the fewest allocations.
  fillSizeClass(state, NB_SMALL_SIZE_CLASSES - 1, true, stats);
}

} // namespace

PyObject* fillPymallocPools() {
  if (!checkMainInterpreter()) {
    return nullptr;
  }
  _obmalloc_state* state = mainObmalloc();
  FillStats stats;
  fill(state, stats);
  auto result = Ref<>::steal(Py_BuildValue("(nn)", stats.blocks, stats.pools));
  // Refill the space the result took, so pools are full on return.
  FillStats result_stats;
  fill(state, result_stats);
  return result.release();
}

#else

PyObject* fillPymallocPools() {
  return Py_BuildValue("(ii)", 0, 0);
}

#endif

} // namespace cinderx
