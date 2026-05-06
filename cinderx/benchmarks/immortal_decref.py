# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""
Immortal decref elision benchmark.

Exercises operations that produce immortal Python singletons (True,
False, None) in tight loops. With the immortal decref optimization,
the JIT elides Incref/Decref sequences for these results entirely.
Without it, each boolean or None result incurs a 7-11 instruction
refcount sequence.

The workloads are:
  - membership tests (``in`` / ``not in``)
  - boolean negation (``not``)
  - typed comparisons (float, int, str)
  - list extend (produces None)
"""

from __future__ import annotations

import sys
import time

import cinderx.jit


def bench_membership(n: int) -> int:
    s = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89}
    count = 0
    for i in range(n):
        if i % 100 in s:
            count += 1
    return count


def bench_not(n: int) -> int:
    count = 0
    flag = False
    for _ in range(n):
        flag = not flag
        if flag:
            count += 1
    return count


def bench_float_compare(n: int) -> int:
    count = 0
    a = 1.0
    b = 2.0
    for _ in range(n):
        if a < b:
            count += 1
        a, b = b, a
    return count


def bench_int_compare(n: int) -> int:
    count = 0
    vals = list(range(100))
    for i in range(n):
        if vals[i % 100] < 50:
            count += 1
    return count


def bench_str_compare(n: int) -> int:
    count = 0
    keys = ["alpha", "beta", "gamma", "delta", "epsilon"]
    target = "gamma"
    for i in range(n):
        if keys[i % 5] == target:
            count += 1
    return count


def bench_list_extend(n: int) -> int:
    chunk = [0, 1, 2, 3, 4]
    total = 0
    for _ in range(n):
        result: list[int] = []
        result.extend(chunk)
        total += len(result)
    return total


BENCHMARKS: list[tuple[str, object]] = [
    ("membership (in)", bench_membership),
    ("boolean (not)", bench_not),
    ("float compare", bench_float_compare),
    ("int compare", bench_int_compare),
    ("str compare", bench_str_compare),
    ("list extend", bench_list_extend),
]


def run_one(name: str, func: object, n: int, warmup: int) -> float:
    assert callable(func)
    for _ in range(warmup):
        func(n)

    trials = 5
    best = float("inf")
    for _ in range(trials):
        t0 = time.perf_counter()
        func(n)
        elapsed = time.perf_counter() - t0
        if elapsed < best:
            best = elapsed
    return best


class ImmortalDecref:
    def run(self, iterations: int) -> bool:
        n = 2_000_000
        for _ in range(iterations):
            for _name, func in BENCHMARKS:
                assert callable(func)
                func(n)
        return True


if __name__ == "__main__":
    cinderx.jit.auto()

    n = 5_000_000
    warmup = 3
    if len(sys.argv) > 1:
        n = int(sys.argv[1])

    print(f"{'Benchmark':<20} {'Time (ms)':>10}")
    print("-" * 32)
    for name, func in BENCHMARKS:
        elapsed = run_one(name, func, n, warmup)
        print(f"{name:<20} {elapsed * 1000:>10.2f}")
