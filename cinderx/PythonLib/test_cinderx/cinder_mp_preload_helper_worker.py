#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Triggered inside each process worker.  The body intentionally allocates and
# discards lots of nested functions to exercise PyFunction_EVENT_DESTROY
# events firing the unit_deleted_during_preload callback during JIT-compiled
# execution.


def churn(n: int) -> int:
    funcs = []
    for _ in range(n):

        def inner(x: int = 0) -> int:
            return x + 1

        funcs.append(inner)
    total = sum(f() for f in funcs)
    del funcs
    return total
