# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Measure repeated Python function creation with and without the CinderX JIT.

The default workload deliberately creates short-lived lambdas inside a
JIT-compiled loop. Each construction fires Python's function lifecycle watchers,
making this a regression benchmark for CinderX's per-function scheduling
bookkeeping.

--nested selects a variant where each created function is also called, so it
gets JIT-compiled, and is then dropped before the next one is created. That
measures the path where a new instance of a nested function has to pick up the
compile made for an earlier instance: if the compile is not kept across the gap
between instances, every iteration pays for a fresh compile instead.
"""

from __future__ import annotations

import argparse
import gc
import os
import statistics
import subprocess
import sys
import time

try:
    import cinderx.jit
except ImportError:
    cinderx = None


SUBPROCESS_ENV_KEYS: tuple[str, ...] = (
    "HOME",
    "LANG",
    "LC_ALL",
    "LD_LIBRARY_PATH",
    "PATH",
    "PYTHONPATH",
    "TMPDIR",
    "VIRTUAL_ENV",
)

CONSTRUCTIONS_PER_ITERATION: int = 8


def construct_lambdas(iterations: int) -> None:
    for _ in range(iterations):
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1
        lambda value: value + 1


def make_nested() -> object:
    def nested(value: int) -> int:
        return value + 1

    return nested


def construct_and_call_nested(iterations: int) -> None:
    for _ in range(iterations):
        # pyre-ignore[29]: the whole point is calling the fresh instance.
        make_nested()(1)


def run(iterations: int, warmup: int, repeat: int, nested: bool = False) -> list[float]:
    workload = construct_and_call_nested if nested else construct_lambdas
    per_iteration = 1 if nested else CONSTRUCTIONS_PER_ITERATION
    workload(warmup)
    if cinderx is not None:
        cinderx.jit.wait_for_background_compiles()
    gc.collect()

    gc_was_enabled = gc.isenabled()
    gc.disable()
    samples_ns: list[float] = []
    try:
        for run_number in range(1, repeat + 1):
            start = time.perf_counter()
            workload(iterations)
            elapsed = time.perf_counter() - start
            per_lambda_ns = elapsed / (iterations * per_iteration) * 1e9
            samples_ns.append(per_lambda_ns)
            print(
                f"  Run {run_number}/{repeat}: {per_lambda_ns:.2f} ns/lambda",
                file=sys.stderr,
            )
    finally:
        if gc_was_enabled:
            gc.enable()

    return samples_ns


def reexec_prefix() -> list[str]:
    if "/xarfuse/" in os.path.abspath(__file__) or sys.argv[0].endswith(
        (".par", ".xar")
    ):
        return [sys.argv[0]]
    return [sys.executable, os.path.abspath(__file__)]


def build_subprocess_env(cinderx_disabled: bool) -> dict[str, str]:
    env = {key: os.environ[key] for key in SUBPROCESS_ENV_KEYS if key in os.environ}
    if cinderx_disabled:
        env["CINDERX_DISABLE"] = "1"
    return env


def measure_subprocess(
    label: str,
    args: argparse.Namespace,
    *,
    enable_cinderx: bool,
) -> float:
    command = [
        *reexec_prefix(),
        "--iterations",
        str(args.iterations),
        "--warmup",
        str(args.warmup),
        "--repeat",
        str(args.repeat),
    ]
    if enable_cinderx:
        command.append("--cinderx")
    if args.nested:
        command.append("--nested")

    print(f"\n--- {label} ---")
    completed = subprocess.run(
        command,
        env=build_subprocess_env(cinderx_disabled=not enable_cinderx),
        capture_output=True,
        text=True,
        check=False,
    )
    sys.stdout.write(completed.stderr)
    sys.stdout.write(completed.stdout)
    if completed.returncode:
        raise SystemExit(completed.returncode)

    for line in completed.stderr.splitlines():
        if line.startswith("result:"):
            return float(line.split()[1])
    raise RuntimeError(f"could not parse result from {label} run")


def compare(args: argparse.Namespace) -> None:
    baseline_ns = measure_subprocess(
        "Baseline (CINDERX_DISABLE=1)", args, enable_cinderx=False
    )
    jit_ns = measure_subprocess("CinderX JIT", args, enable_cinderx=True)

    workload = "nested construct+call" if args.nested else "lambda construction"
    print("\n" + "=" * 60)
    print(f"{'workload':<24}{'baseline':>14}{'jit':>14}{'jit/base':>10}")
    print("-" * 60)
    print(
        f"{workload:<24}"
        f"{baseline_ns:>11.2f} ns"
        f"{jit_ns:>11.2f} ns"
        f"{jit_ns / baseline_ns:>9.2f}x"
    )
    print("=" * 60 + "  (lower is better)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cinderx", action="store_true", help="Enable and force the CinderX JIT"
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        help="Run interpreter and JIT subprocesses and compare them",
    )
    parser.add_argument(
        "--nested",
        action="store_true",
        help="Measure creating, calling and dropping a nested function instead, "
        "which needs the compile to survive between instances",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=250_000,
        help="Loop iterations per timed run (eight constructions each, or one "
        "construct+call with --nested)",
    )
    parser.add_argument(
        "--warmup", type=int, default=10_000, help="Loop iterations before timing"
    )
    parser.add_argument("--repeat", type=int, default=7, help="Number of timed runs")
    args = parser.parse_args()
    for name in ("iterations", "warmup", "repeat"):
        if getattr(args, name) < 1:
            parser.error(f"--{name} must be positive")
    return args


def main() -> None:
    args = parse_args()
    if args.compare:
        compare(args)
        return

    if cinderx is None:
        args.cinderx = False

    driver = construct_and_call_nested if args.nested else construct_lambdas
    if args.cinderx and cinderx is not None:
        cinderx.jit.auto()
        cinderx.jit.force_compile(driver)

    samples_ns = run(args.iterations, args.warmup, args.repeat, args.nested)
    median_ns = statistics.median(samples_ns)
    print(f"Python {sys.version.split()[0]}", file=sys.stderr)
    compiled = False
    if cinderx:
        compiled = cinderx.jit.is_jit_compiled(driver)
    print(
        f"JIT requested={'yes' if args.cinderx else 'no'} compiled={compiled}",
        file=sys.stderr,
    )
    print(f"median: {median_ns:.2f} ns/lambda", file=sys.stderr)
    print(f"result: {median_ns:.4f}", file=sys.stderr)


if __name__ == "__main__":
    main()
