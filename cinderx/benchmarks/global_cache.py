# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Benchmark module-global loads across cache lifecycle transitions.

The benchmark crosses two workload shapes with four cache states:

* ``cached`` binds globals before compilation.
* ``bound-after-compile`` first binds globals after compilation.
* ``same-type-rebind`` replaces globals with same-type values after compilation.
* ``post-uncompile`` mutates globals in the interpreter after JIT compilation and
  forced uncompile, exposing cache-watcher overhead left behind by compiled code.

``direct`` is modeled after the B-style global/table workloads, while
``heavy-scalar`` reads 26 integer globals directly in every loop iteration.
Results are median nanoseconds per module-global load.
"""

from __future__ import annotations

import argparse
import gc
import statistics
import sys
import time
from dataclasses import dataclass
from typing import Callable, cast

import cinderx.jit


DEFAULT_LOOPS: int = 250_000
DEFAULT_WARMUP_LOOPS: int = 10_000

DIRECT_NAMES: tuple[str, ...] = tuple(f"GLOBAL_{letter}" for letter in "ABCDE")
SCALAR_NAMES: tuple[str, ...] = tuple(
    f"GLOBAL_{letter}" for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
)
SCALAR_VALUES: tuple[int, ...] = (
    3,
    5,
    7,
    11,
    13,
    17,
    19,
    23,
    29,
    31,
    37,
    41,
    43,
    47,
    53,
    59,
    61,
    67,
    71,
    73,
    79,
    83,
    89,
    97,
    101,
    103,
)


@dataclass(frozen=True)
class Workload:
    name: str
    family: str
    pattern: str
    description: str
    loads_per_loop: int


@dataclass(frozen=True)
class Benchmark:
    workload: Workload
    fn: Callable[[int], int]
    namespace: dict[str, object]
    expected_compiled: bool


PATTERNS: tuple[tuple[str, str], ...] = (
    ("cached", "globals bound and cached at compile time"),
    ("bound-after-compile", "globals first bound after compilation"),
    ("same-type-rebind", "same-type global replacement after compilation"),
    ("post-uncompile", "interpreted mutation after forced uncompile"),
)

FAMILIES: tuple[tuple[str, str, int], ...] = (
    ("direct", "B-style table and scalar reads", 14),
    ("heavy-scalar", "26 direct scalar-global reads", 32),
)

WORKLOADS: dict[str, Workload] = {
    f"{family}-{pattern}": Workload(
        name=f"{family}-{pattern}",
        family=family,
        pattern=pattern,
        description=f"{family_description}; {pattern_description}",
        loads_per_loop=loads_per_loop,
    )
    for family, family_description, loads_per_loop in FAMILIES
    for pattern, pattern_description in PATTERNS
}


def _hot_source(family: str) -> str:
    if family == "direct":
        mixed = "\n            + ".join(("GLOBAL_TABLE[idx]", *DIRECT_NAMES))
        prefix = "        idx = (i * GLOBAL_STEP + GLOBAL_ACC) & 511\n"
    else:
        mixed = "\n            + ".join(SCALAR_NAMES)
        prefix = ""
    return (
        "def hot(work_size):\n"
        "    global GLOBAL_ACC\n"
        "    acc = GLOBAL_ACC\n"
        "    for i in range(work_size):\n"
        f"{prefix}"
        "        mixed = (\n"
        f"            {mixed}\n"
        "        ) & GLOBAL_MASK\n"
        "        acc = (acc + mixed) & GLOBAL_MASK\n"
        "        GLOBAL_ACC = (GLOBAL_ACC + GLOBAL_D + (acc & 31)) & GLOBAL_MASK\n"
        "        acc ^= GLOBAL_ACC\n"
        "    GLOBAL_ACC = acc\n"
        "    return acc\n"
    )


def _bind_globals(namespace: dict[str, object], family: str, seed: int) -> None:
    offset = seed * 1000
    mask = (1 << 64) - 1
    namespace["GLOBAL_ACC"] = 0
    namespace["GLOBAL_MASK"] = mask
    for name, value in zip(SCALAR_NAMES, SCALAR_VALUES):
        namespace[name] = value + offset
    if family == "direct":
        namespace["GLOBAL_STEP"] = 17 + offset
        namespace["GLOBAL_TABLE"] = [
            ((i * 1315423911) ^ (i << 11) ^ (i >> 3)) & mask for i in range(512)
        ]


def _compile(fn: Callable[[int], int]) -> bool:
    if not cinderx.jit.is_enabled():
        return False
    if not cinderx.jit.force_compile(fn):
        raise RuntimeError(f"JIT refused to compile {fn.__name__}")
    return True


def build_benchmark(workload: Workload) -> Benchmark:
    namespace: dict[str, object] = {"__name__": f"global_cache_{workload.name}"}
    exec(
        compile(
            _hot_source(workload.family), f"<global_cache:{workload.name}>", "exec"
        ),
        namespace,
    )
    fn = cast(Callable[[int], int], namespace["hot"])

    if workload.pattern == "bound-after-compile":
        compiled = _compile(fn)
        _bind_globals(namespace, workload.family, seed=0)
    else:
        _bind_globals(namespace, workload.family, seed=0)
        compiled = _compile(fn)

    if workload.pattern == "same-type-rebind":
        _bind_globals(namespace, workload.family, seed=1)
    elif workload.pattern == "post-uncompile" and compiled:
        if not cinderx.jit.force_uncompile(fn):
            raise RuntimeError(f"JIT refused to uncompile {fn.__name__}")
        cinderx.jit.jit_suppress(fn)
        compiled = False

    return Benchmark(workload, fn, namespace, compiled)


def _deopt_count() -> int:
    stats = cinderx.jit.get_and_clear_runtime_stats()
    deopts = stats.get("deopt", [])
    if not isinstance(deopts, list):
        return 0
    count = 0
    for deopt in deopts:
        if not isinstance(deopt, dict):
            continue
        integers = deopt.get("int", {})
        if isinstance(integers, dict):
            value = integers.get("count", 0)
            if isinstance(value, int):
                count += value
    return count


def run(
    benchmark: Benchmark, loops: int, warmup_loops: int, repeat: int
) -> tuple[list[float], int, int]:
    cinderx.jit.clear_runtime_stats()
    benchmark.namespace["GLOBAL_ACC"] = 0
    benchmark.fn(warmup_loops)
    cinderx.jit.wait_for_background_compiles()
    gc.collect()

    samples: list[float] = []
    checksum = 0
    for _ in range(repeat):
        benchmark.namespace["GLOBAL_ACC"] = 0
        start = time.perf_counter()
        checksum = benchmark.fn(loops)
        elapsed = time.perf_counter() - start
        samples.append(elapsed / (loops * benchmark.workload.loads_per_loop) * 1e9)
    return samples, checksum, _deopt_count()


def print_result(
    benchmark: Benchmark,
    loops: int,
    warmup_loops: int,
    samples: list[float],
    checksum: int,
    deopts: int,
) -> None:
    median = statistics.median(samples)
    compiled = cinderx.jit.is_jit_compiled(benchmark.fn)
    if compiled != benchmark.expected_compiled:
        raise RuntimeError(
            f"{benchmark.workload.name}: expected compiled={benchmark.expected_compiled}, "
            f"got {compiled}"
        )
    print(f"\nworkload={benchmark.workload.name}", file=sys.stderr)
    print(f"  {benchmark.workload.description}", file=sys.stderr)
    print(
        f"  warmup_loops={warmup_loops} loops={loops} runs={len(samples)} "
        f"global_loads_per_loop={benchmark.workload.loads_per_loop}",
        file=sys.stderr,
    )
    print(f"  run times: {[f'{sample:.2f}ns' for sample in samples]}", file=sys.stderr)
    print(
        f"  median: {median:.2f} ns/global-load compiled={compiled} "
        f"deopts={deopts} checksum={checksum}",
        file=sys.stderr,
    )
    print(f"result: {benchmark.workload.name} {median:.4f}", file=sys.stderr)


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "iterations",
        nargs="?",
        type=_positive_int,
        default=3,
        help="number of timed samples per workload (default: 3)",
    )
    parser.add_argument(
        "--workload",
        choices=tuple(WORKLOADS),
        help="run one workload instead of all eight",
    )
    parser.add_argument("--loops", type=_positive_int, default=DEFAULT_LOOPS)
    parser.add_argument(
        "--warmup-loops", type=_positive_int, default=DEFAULT_WARMUP_LOOPS
    )
    parser.add_argument("--list", action="store_true", help="list workload names")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.list:
        for name, workload in WORKLOADS.items():
            print(f"{name:<36} {workload.description}")
        return

    cinderx.jit.auto()
    names = [args.workload] if args.workload else list(WORKLOADS)
    print(
        f"Python {sys.version.split()[0]} CinderX JIT enabled={cinderx.jit.is_enabled()}",
        file=sys.stderr,
    )
    for name in names:
        benchmark = build_benchmark(WORKLOADS[name])
        samples, checksum, deopts = run(
            benchmark, args.loops, args.warmup_loops, args.iterations
        )
        print_result(
            benchmark, args.loops, args.warmup_loops, samples, checksum, deopts
        )


if __name__ == "__main__":
    main()
