#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import multiprocessing
import sys


def _child(_: int) -> int:
    # Imported lazily *inside* the child so that the lazy-import + JIT preload
    # code path runs after the fork.  Each child:
    #
    #   1. Boots the JIT (cinderx auto-installs from -X jit-all in args).
    #
    #   2. Triggers compilation on a JIT-eligible function by calling it.
    #
    #   3. The callee allocates and drops many functions, firing
    #      PyFunction_EVENT_DESTROY events during JIT-compiled execution and
    #      during compilation and preloading.
    import cinder_mp_preload_helper_worker as worker

    total = 0
    for _ in range(8):
        total += worker.churn(200)
    return total


def main() -> int:
    # Use the "fork" start method explicitly so the child inherits any JIT +
    # Lazy Imports state the parent may have set up while loading cinderx.
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(processes=4) as pool:
        results = pool.map(_child, range(8))
    print("ok", sum(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
