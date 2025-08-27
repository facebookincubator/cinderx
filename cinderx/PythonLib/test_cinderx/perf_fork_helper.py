#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import multiprocessing
import os
import sys

import cinderx.jit


def compute():
    n = 0
    for i in range(10):
        n += i
    return n


def child1():
    print(f"child1({os.getpid()}) computed {compute()}")


def child2():
    print(f"child2({os.getpid()}) computed {compute()}")


def parent():
    print(f"parent({os.getpid()}) computed {compute()}")


def main():
    queue = multiprocessing.Queue(2)

    pid1 = os.fork()
    if pid1 == 0:
        queue.put("child1", False)
        sys.exit(child1())

    pid2 = os.fork()
    if pid2 == 0:
        queue.put("child2", False)
        sys.exit(child2())

    read = {queue.get(True, timeout=2), queue.get(True, timeout=2)}
    if read != {"child1", "child2"}:
        raise RuntimeError(f"Unexpected message queue contents: {read}")

    parent()
    os.waitpid(pid1, 0)
    os.waitpid(pid2, 0)


@cinderx.jit.jit_suppress
def schedule_compilation() -> None:
    """
    Set up functions to be compiled when they're first called.  Processes that don't
    call them will leave them uncompiled.
    """

    cinderx.jit.lazy_compile(main)
    cinderx.jit.lazy_compile(parent)
    cinderx.jit.lazy_compile(child1)
    cinderx.jit.lazy_compile(child2)
    cinderx.jit.lazy_compile(compute)


if __name__ == "__main__":
    schedule_compilation()
    sys.exit(main())
