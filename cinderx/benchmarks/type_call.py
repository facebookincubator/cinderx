# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Benchmark zero-argument type calls across base types and slot overrides."""

from __future__ import annotations

import argparse
import gc
import json
import os
import statistics
import subprocess
import sys
import time
from collections.abc import Callable
from typing import Any

try:
    import cinderx.jit as jit
except ImportError:
    jit = None


BASES: dict[str, type] = {"object": object, "list": list, "tuple": tuple}
SHAPES: dict[str, str] = {
    "plain": "    pass\n",
    "new": "    def __new__(cls):\n        return base.__new__(cls)\n",
    "init": "    def __init__(self):\n        base.__init__(self)\n",
    "both": (
        "    def __new__(cls):\n        return base.__new__(cls)\n"
        "    def __init__(self):\n        base.__init__(self)\n"
    ),
}
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


def workloads() -> dict[str, Callable[[int], None]]:
    result: dict[str, Callable[[int], None]] = {}
    for base_name, base in BASES.items():
        for shape, body in SHAPES.items():
            namespace: dict[str, Any] = {"base": base}
            # Each loop has a fixed global type, not a type passed as an argument.
            exec(
                "class Benchmark(base):\n"
                + body
                + "\ndef construct(iterations: int) -> None:\n"
                "    for _ in range(iterations):\n"
                "        Benchmark()\n",
                namespace,
            )
            result[f"{base_name}/{shape}"] = namespace["construct"]
    return result


def measure(args: argparse.Namespace) -> dict[str, float]:
    cases = workloads()
    if args.cinderx:
        if jit is None or not jit.is_enabled():
            raise RuntimeError("CinderX JIT is unavailable or disabled")
        jit.auto()
        for workload in cases.values():
            if not jit.force_compile(workload):
                raise RuntimeError("could not compile benchmark workload")
    results: dict[str, float] = {}
    for name, workload in cases.items():
        workload(args.warmup)
        if jit is not None:
            jit.wait_for_background_compiles()
        gc.collect()
        gc_was_enabled = gc.isenabled()
        gc.disable()
        samples: list[float] = []
        try:
            for _ in range(args.repeat):
                start = time.perf_counter_ns()
                workload(args.iterations)
                samples.append((time.perf_counter_ns() - start) / args.iterations)
        finally:
            if gc_was_enabled:
                gc.enable()
        results[name] = statistics.median(samples)
    return results


def measure_subprocess(
    args: argparse.Namespace, *, enable_jit: bool
) -> dict[str, float]:
    if "/xarfuse/" in os.path.abspath(__file__) or sys.argv[0].endswith(
        (".par", ".xar")
    ):
        command = [sys.argv[0]]
    else:
        command = [sys.executable, os.path.abspath(__file__)]
    command.extend(
        [
            "--iterations",
            str(args.iterations),
            "--warmup",
            str(args.warmup),
            "--repeat",
            str(args.repeat),
            "--json",
        ]
    )
    env = {key: os.environ[key] for key in SUBPROCESS_ENV_KEYS if key in os.environ}
    if enable_jit:
        command.append("--cinderx")
    else:
        env["CINDERX_DISABLE"] = "1"
    completed = subprocess.run(
        command, env=env, capture_output=True, text=True, check=True
    )
    sys.stderr.write(completed.stderr)
    return json.loads(completed.stdout)


def compare(args: argparse.Namespace) -> None:
    baseline = measure_subprocess(args, enable_jit=False)
    compiled = measure_subprocess(args, enable_jit=True)
    print(f"{'case':<16}{'baseline ns':>14}{'JIT ns':>14}{'JIT/base':>12}")
    for name, baseline_ns in baseline.items():
        jit_ns = compiled[name]
        print(
            f"{name:<16}{baseline_ns:>14.2f}{jit_ns:>14.2f}"
            f"{jit_ns / baseline_ns:>12.2f}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--describe", action="store_true", help="List benchmark cases")
    parser.add_argument(
        "--compare", action="store_true", help="Compare interpreter and JIT"
    )
    parser.add_argument("--cinderx", action="store_true", help="Force JIT compilation")
    parser.add_argument(
        "--json", action="store_true", help="Print machine-readable timings"
    )
    parser.add_argument("--iterations", type=int, default=250_000)
    parser.add_argument("--warmup", type=int, default=10_000)
    parser.add_argument("--repeat", type=int, default=7)
    args = parser.parse_args()
    for name in ("iterations", "warmup", "repeat"):
        if getattr(args, name) < 1:
            parser.error(f"--{name} must be positive")
    return args


def main() -> None:
    args = parse_args()
    if args.describe:
        for name in workloads():
            path = (
                "TpAlloc candidate"
                if name == "object/plain"
                else "generic call control"
            )
            print(f"{name}: {path}")
        return
    if args.compare:
        compare(args)
        return
    results = measure(args)
    if args.json:
        print(json.dumps(results))
    else:
        for name, nanoseconds in results.items():
            print(f"{name}: {nanoseconds:.2f} ns/call")


if __name__ == "__main__":
    main()
