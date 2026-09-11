#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Helper for test_jit_perf_map entry-inheritance tests.

The parent JIT-compiles and runs `before_fork`, then forks once. The child
never calls `before_fork`; it only runs `child_only`. The parent test reads
`/tmp/perf-<pid>.map` for each printed pid and asserts:

* the child's map contains the parent's pre-fork `before_fork` entry (the
  child gets a copy of whatever the parent had at fork time);
* the parent's map does not contain the child's `child_only` entry.
"""

import os
import sys
import time
from typing import Callable

import cinderx.jit


def before_fork() -> int:
    n = 0
    for i in range(10):
        n += i
    return n


def child_only() -> int:
    n = 1
    for i in range(10):
        n *= i + 1
    return n


def wait_until_compiled(func: Callable[[], int], timeout_sec: float = 120.0) -> None:
    deadline = time.monotonic() + timeout_sec
    while not cinderx.jit.is_jit_compiled(func):
        if time.monotonic() > deadline:
            raise RuntimeError(f"{func!r} was not JIT-compiled in time")
        time.sleep(0.05)


def main() -> int:
    cinderx.jit.lazy_compile(before_fork)
    cinderx.jit.lazy_compile(child_only)
    before_fork()
    # Poll rather than assuming synchronous compilation so this works whether
    # or not background compilation is enabled.
    wait_until_compiled(before_fork)

    pid = os.fork()
    if pid == 0:
        # Child: must not call before_fork; its map should still contain the
        # parent's entry via the post-fork copy.
        result = child_only()
        wait_until_compiled(child_only)
        print(f"child({os.getpid()}) computed {result}", flush=True)
        return 0

    print(f"parent({os.getpid()}) computed {before_fork()}", flush=True)
    _, status = os.waitpid(pid, 0)
    if status != 0:
        raise RuntimeError(f"child {pid} exited with status {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
