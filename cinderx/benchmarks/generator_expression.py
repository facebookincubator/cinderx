# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Measure generator-expression overhead with and without the CinderX JIT."""

from __future__ import annotations

import argparse
import gc
import os
import statistics
import subprocess
import sys
import time
import types
from collections.abc import Callable, Iterable

try:
    import cinderx.jit
except ImportError:
    cinderx = None


SUBPROCESS_ENV_KEYS: tuple[str, ...] = (
    "CINDERX_JIT_UNLINKED_GENERATOR_CONSTRUCTION",
    "HOME",
    "LANG",
    "LC_ALL",
    "LD_LIBRARY_PATH",
    "PATH",
    "PYTHONPATH",
    "TMPDIR",
    "VIRTUAL_ENV",
)


def consume(values: Iterable[int]) -> int:
    return sum(values)


def consume_genexpr(iterations: int, width: int) -> int:
    total = 0
    for _ in range(iterations):
        total += consume(value * value for value in range(width))
    return total


def create_genexpr(iterations: int, width: int) -> int:
    generator = None
    for _ in range(iterations):
        generator = (value * value for value in range(width))
    return iterations if generator is not None else 0


def consume_listcomp(iterations: int, width: int) -> int:
    total = 0
    for _ in range(iterations):
        total += consume([value * value for value in range(width)])
    return total


def genexpr_code(function: Callable[[int, int], int]) -> types.CodeType:
    for constant in function.__code__.co_consts:
        if isinstance(constant, types.CodeType) and constant.co_name == "<genexpr>":
            return constant
    raise RuntimeError("could not find generator-expression code object")


def expected_result(iterations: int, width: int) -> int:
    return iterations * (width - 1) * width * (2 * width - 1) // 6


def measure(
    workload: Callable[[int, int], int],
    iterations: int,
    width: int,
    warmup: int,
    repeat: int,
    expected: int,
    clock: Callable[[], float],
) -> list[float]:
    workload(warmup, width)
    if cinderx is not None:
        cinderx.jit.wait_for_background_compiles()
    gc.collect()

    gc_was_enabled = gc.isenabled()
    gc.disable()
    samples_ns: list[float] = []
    try:
        for run_number in range(1, repeat + 1):
            start = clock()
            result = workload(iterations, width)
            elapsed = clock() - start
            if result != expected:
                raise RuntimeError(f"unexpected result from {workload.__name__}")
            per_expression_ns = elapsed / iterations * 1e9
            samples_ns.append(per_expression_ns)
            print(
                f"  Run {run_number}/{repeat}: {per_expression_ns:.2f} ns/expression",
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
) -> dict[str, float]:
    command = [
        *reexec_prefix(),
        "--iterations",
        str(args.iterations),
        "--width",
        str(args.width),
        "--warmup",
        str(args.warmup),
        "--repeat",
        str(args.repeat),
        "--clock",
        args.clock,
    ]
    if enable_cinderx:
        command.append("--cinderx")

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

    results: dict[str, float] = {}
    for line in completed.stderr.splitlines():
        if line.startswith("result:"):
            _, name, value = line.split()
            results[name] = float(value)
    if len(results) != 3:
        raise RuntimeError(f"could not parse results from {label} run")
    return results


def compare(args: argparse.Namespace) -> None:
    baseline = measure_subprocess(
        "Baseline (CINDERX_DISABLE=1)", args, enable_cinderx=False
    )
    jit = measure_subprocess("CinderX JIT", args, enable_cinderx=True)

    print("\n" + "=" * 68)
    print(f"{'workload':<28}{'baseline':>14}{'jit':>14}{'jit/base':>10}")
    print("-" * 68)
    for name, label in (
        ("create_genexpr", "generator creation only"),
        ("genexpr", "generator expression"),
        ("listcomp", "list comprehension"),
    ):
        print(
            f"{label:<28}"
            f"{baseline[name]:>11.2f} ns"
            f"{jit[name]:>11.2f} ns"
            f"{jit[name] / baseline[name]:>9.2f}x"
        )
    print("=" * 68 + "  (lower is better)")


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
        "--iterations",
        type=int,
        default=250_000,
        help="Expressions created per timed run",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=1,
        help="Items produced by each expression",
    )
    parser.add_argument(
        "--warmup", type=int, default=10_000, help="Expressions created before timing"
    )
    parser.add_argument("--repeat", type=int, default=7, help="Number of timed runs")
    parser.add_argument(
        "--clock",
        choices=("wall", "process"),
        default="wall",
        help="Timer to use (wall clock or process CPU time)",
    )
    args = parser.parse_args()
    for name in ("iterations", "width", "warmup", "repeat"):
        if getattr(args, name) < 1:
            parser.error(f"--{name} must be positive")
    return args


def main() -> None:
    args = parse_args()
    if args.compare:
        compare(args)
        return

    creation_genexpr_probe = types.FunctionType(genexpr_code(create_genexpr), globals())
    consumed_genexpr_probe = types.FunctionType(
        genexpr_code(consume_genexpr), globals()
    )
    if cinderx is None:
        args.cinderx = False
    if args.cinderx:
        assert cinderx is not None
        cinderx.jit.auto()
        for function in (
            consume,
            create_genexpr,
            consume_genexpr,
            consume_listcomp,
            creation_genexpr_probe,
            consumed_genexpr_probe,
        ):
            cinderx.jit.force_compile(function)

    consumed_result = expected_result(args.iterations, args.width)
    clock = time.process_time if args.clock == "process" else time.perf_counter
    for name, label, workload, expected in (
        ("create_genexpr", "generator creation only", create_genexpr, args.iterations),
        ("genexpr", "generator expression", consume_genexpr, consumed_result),
        ("listcomp", "list comprehension", consume_listcomp, consumed_result),
    ):
        samples_ns = measure(
            workload,
            args.iterations,
            args.width,
            args.warmup,
            args.repeat,
            expected,
            clock,
        )
        median_ns = statistics.median(samples_ns)
        print(f"median {label}: {median_ns:.2f} ns/expression", file=sys.stderr)
        print(f"result: {name} {median_ns:.4f}", file=sys.stderr)

    creation_outer_compiled = False
    consumed_outer_compiled = False
    creation_genexpr_compiled = False
    consumed_genexpr_compiled = False
    if cinderx is not None:
        creation_outer_compiled = cinderx.jit.is_jit_compiled(create_genexpr)
        consumed_outer_compiled = cinderx.jit.is_jit_compiled(consume_genexpr)
        creation_genexpr_compiled = cinderx.jit.is_jit_compiled(creation_genexpr_probe)
        consumed_genexpr_compiled = cinderx.jit.is_jit_compiled(consumed_genexpr_probe)

    print(f"Python {sys.version.split()[0]}", file=sys.stderr)
    print(
        f"JIT requested={'yes' if args.cinderx else 'no'} "
        f"creation outer compiled={creation_outer_compiled} "
        f"consumed outer compiled={consumed_outer_compiled} "
        f"creation genexpr compiled={creation_genexpr_compiled} "
        f"consumed genexpr compiled={consumed_genexpr_compiled}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
