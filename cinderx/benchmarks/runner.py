# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""
Unified benchmark runner for CinderX.

Usage:
    python benchmarks/runner.py              # Run all available benchmarks
    python benchmarks/runner.py binary_trees  # Run specific benchmark(s)
    python benchmarks/runner.py --list        # List available benchmarks
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import time

# Lightweight benchmarks that only require cinderx itself.
LIGHTWEIGHT_BENCHMARKS: list[str] = [
    "binary_trees",
    "fannkuch",
    "nbody",
    "richards",
    "spectral_norm",
]

# JIT compilation time benchmarks (measure compilation speed, not runtime).
JIT_COMPILATION_BENCHMARKS: list[str] = [
    "compile_time",
]

# Heavyweight benchmarks with extra dependencies.
HEAVYWEIGHT_BENCHMARKS: dict[str, str] = {
    "fastmark": "requirements-fastmark.txt",
    "torchbench": "requirements-torchbench.txt",
}

ALL_BENCHMARK_NAMES: list[str] = (
    LIGHTWEIGHT_BENCHMARKS
    + JIT_COMPILATION_BENCHMARKS
    + list(HEAVYWEIGHT_BENCHMARKS.keys())
)


def run_benchmark(
    name: str, iterations: int, extra_args: list[str]
) -> tuple[bool, float | None]:
    """Run a single benchmark as a subprocess.

    `extra_args` is forwarded verbatim to the underlying benchmark script,
    letting callers override defaults (e.g. `--scale` for fastmark).

    Returns (success, elapsed_seconds). elapsed_seconds is None if the
    subprocess could not be launched at all.
    """
    benchmarks_dir = os.path.dirname(os.path.abspath(__file__))

    if name in LIGHTWEIGHT_BENCHMARKS:
        script = os.path.join(benchmarks_dir, f"{name}.py")
        cmd = [sys.executable, script, str(iterations), *extra_args]
    elif name == "compile_time":
        script = os.path.join(benchmarks_dir, "compile_time.py")
        cmd = [sys.executable, script, *extra_args]
    elif name == "fastmark":
        script = os.path.join(benchmarks_dir, "fastmark.py")
        cmd = [sys.executable, script, "--cinderx", *extra_args]
    elif name == "torchbench":
        script = os.path.join(benchmarks_dir, "torchbench.py")
        cmd = [sys.executable, script, "--cinderx", *extra_args]
    else:
        print(f"Unknown benchmark: {name}", file=sys.stderr)
        return False, None

    print(f"\n{'=' * 60}")
    print(f"Running: {name}")
    print(f"{'=' * 60}")

    start = time.perf_counter()
    try:
        result = subprocess.run(cmd, check=False)
    except FileNotFoundError:
        print(f"  Error: could not execute {cmd[0]}", file=sys.stderr)
        return False, None
    elapsed = time.perf_counter() - start

    print(f"  {name} finished in {elapsed:.2f}s")
    return result.returncode == 0, elapsed


def check_deps(name: str) -> bool:
    """Check if a benchmark's dependencies are available."""
    if name in LIGHTWEIGHT_BENCHMARKS or name in JIT_COMPILATION_BENCHMARKS:
        return True
    elif name == "fastmark":
        return importlib.util.find_spec("pyperformance") is not None
    elif name == "torchbench":
        return importlib.util.find_spec("torchbenchmark") is not None
    return True


def print_list() -> None:
    print("Available benchmarks:")
    print()
    print("Lightweight (no extra dependencies):")
    for name in LIGHTWEIGHT_BENCHMARKS:
        print(f"  {name}")
    print()
    print("JIT compilation time:")
    for name in JIT_COMPILATION_BENCHMARKS:
        print(f"  {name}")
    print()
    print("Heavyweight (extra dependencies required):")
    for name, req in HEAVYWEIGHT_BENCHMARKS.items():
        print(f"  {name}  (uv pip install -r {req})")


def print_help() -> None:
    print(__doc__)
    print("Options:")
    print("  --list, -l          List available benchmarks")
    print(
        "  --iterations N      Number of iterations for lightweight benchmarks (default: 3)"
    )
    print(
        "  --                  Forward all subsequent args to the underlying benchmark"
    )
    print("  --help, -h          Show this help message")
    print()
    print("Examples:")
    print("  python benchmarks/runner.py")
    print("  python benchmarks/runner.py binary_trees fannkuch")
    print("  python benchmarks/runner.py compile_time")
    print("  python benchmarks/runner.py fastmark -- --scale 50")


def parse_args(
    argv: list[str],
) -> tuple[int, list[str], list[str]]:
    """Parse runner arguments.

    Returns (iterations, benchmark_names, extra_args). extra_args are forwarded
    to the underlying benchmark script and come from anything after a `--`
    separator.
    """
    iterations = 3
    extra_args: list[str] = []

    if "--" in argv:
        idx = argv.index("--")
        extra_args = argv[idx + 1 :]
        argv = argv[:idx]

    if "--iterations" in argv:
        idx = argv.index("--iterations")
        try:
            iterations = int(argv[idx + 1])
            argv = argv[:idx] + argv[idx + 2 :]
        except (IndexError, ValueError):
            print("Error: --iterations requires an integer argument", file=sys.stderr)
            sys.exit(1)

    return iterations, argv, extra_args


def run_all(
    benchmarks_to_run: list[str], iterations: int, extra_args: list[str]
) -> int:
    """Run the requested benchmarks and print a summary. Returns exit code."""
    passed = 0
    skipped = 0
    failed = 0
    # (name, status, elapsed_seconds_or_None) entries for the summary table.
    summary: list[tuple[str, str, float | None]] = []

    for name in benchmarks_to_run:
        if not check_deps(name):
            req_file = HEAVYWEIGHT_BENCHMARKS.get(name)
            print(f"\nSkipping {name} — missing dependencies.")
            if req_file:
                print(f"  Install with: uv pip install -r benchmarks/{req_file}")
            skipped += 1
            summary.append((name, "SKIP", None))
            continue

        success, elapsed = run_benchmark(name, iterations, extra_args)
        if success:
            passed += 1
            summary.append((name, "PASS", elapsed))
        else:
            failed += 1
            summary.append((name, "FAIL", elapsed))

    print(f"\n{'=' * 60}")
    print("Benchmark summary")
    print(f"{'=' * 60}")
    print(f"{'Benchmark':<20} {'Status':<8} {'Time (s)':>10}")
    print("-" * 40)
    for name, status, elapsed in summary:
        time_str = f"{elapsed:.2f}" if elapsed is not None else "—"
        print(f"{name:<20} {status:<8} {time_str:>10}")
    print("-" * 40)
    print(f"Results: {passed} passed, {skipped} skipped, {failed} failed")
    print(f"{'=' * 60}")

    return 1 if failed else 0


def main() -> None:
    argv = sys.argv[1:]

    if "--list" in argv or "-l" in argv:
        print_list()
        return

    if "--help" in argv or "-h" in argv:
        print_help()
        return

    iterations, names, extra_args = parse_args(argv)
    benchmarks_to_run = names if names else ALL_BENCHMARK_NAMES

    for name in benchmarks_to_run:
        if name not in ALL_BENCHMARK_NAMES:
            print(f"Unknown benchmark: {name}", file=sys.stderr)
            print(
                "Run 'python benchmarks/runner.py --list' to see available benchmarks"
            )
            sys.exit(1)

    sys.exit(run_all(benchmarks_to_run, iterations, extra_args))


if __name__ == "__main__":
    main()
