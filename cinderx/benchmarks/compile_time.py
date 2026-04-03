# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""
JIT compilation speed benchmark.

Measures how long it takes the CinderX JIT to compile functions, not the
runtime performance of the generated code. Uses compile_after_n_calls(0) to
eagerly compile all functions on import, then reports per-function and
aggregate timing statistics.

Usage:
    buck run //cinderx/benchmarks:compile-time

For a per-phase breakdown of each function's compilation, pass the jit-time
flag via Python's -X option:
    buck run //cinderx/benchmarks:compile-time -- -X jit-time='*'
"""

from __future__ import annotations

import logging

import cinderx.jit

logger: logging.Logger = logging.getLogger(__name__)


def main() -> None:
    cinderx.jit.compile_after_n_calls(0)

    # Importing these modules triggers JIT compilation of all their functions.
    # These are bundled via srcs in the BUCK target, not as separate deps.
    from cinderx.benchmarks import (  # noqa: F811  # @manual
        binary_trees,
        fannkuch,
        nbody,
        richards,
        spectral_norm,
    )

    # Suppress unused import warnings.
    _ = (binary_trees, fannkuch, nbody, richards, spectral_norm)

    cinderx.jit.disable()

    results: list[tuple[str, float]] = []
    for func in cinderx.jit.get_compiled_functions():
        comp_time = cinderx.jit.get_function_compilation_time(func)
        name = f"{func.__module__}:{func.__qualname__}"
        results.append((name, comp_time))

    results.sort(key=lambda r: r[1], reverse=True)

    print(f"{'Function':<60} {'Time (ms)':>10}")
    print("-" * 71)
    for name, t in results:
        print(f"{name:<60} {t:>10.3f}")

    total = sum(t for _, t in results)
    print("-" * 71)
    print(f"{'Total':<60} {total:>10.3f}")
    print(f"{'Functions compiled':<60} {len(results):>10}")


if __name__ == "__main__":
    main()
