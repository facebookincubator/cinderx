# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-ignore-all-errors

"""
Unified benchmark runner for CinderX.

Usage:
    python -m cinderx.benchmarks              # Run all available benchmarks
    python -m cinderx.benchmarks binary_trees  # Run specific benchmark(s)
    python -m cinderx.benchmarks --list        # List available benchmarks
"""

import os
import subprocess
import sys

# Lightweight benchmarks that only require cinderx itself.
LIGHTWEIGHT_BENCHMARKS = [
    "binary_trees",
    "fannkuch",
    "nbody",
    "richards",
    "spectral_norm",
]

# JIT compilation time benchmarks (measure compilation speed, not runtime).
JIT_COMPILATION_BENCHMARKS = [
    "compile_time",
]

# Heavyweight benchmarks with extra dependencies.
HEAVYWEIGHT_BENCHMARKS = {
    "fastmark": "requirements-fastmark.txt",
    "torchrec_pt2": "requirements-torchrec.txt",
}

ALL_BENCHMARK_NAMES = (
    LIGHTWEIGHT_BENCHMARKS
    + JIT_COMPILATION_BENCHMARKS
    + list(HEAVYWEIGHT_BENCHMARKS.keys())
)


def run_benchmark(name, iterations):
    """Run a single benchmark as a subprocess. Returns True on success."""
    benchmarks_dir = os.path.dirname(os.path.abspath(__file__))

    if name in LIGHTWEIGHT_BENCHMARKS:
        script = os.path.join(benchmarks_dir, f"{name}.py")
        cmd = [sys.executable, script, str(iterations)]
    elif name == "compile_time":
        cmd = [sys.executable, "-m", "cinderx.benchmarks.compile_time"]
    elif name == "fastmark":
        script = os.path.join(benchmarks_dir, "fastmark.py")
        cmd = [sys.executable, script, "--cinderx", "--scale", "10"]
    elif name == "torchrec_pt2":
        script = os.path.join(benchmarks_dir, "torchrec_pt2", "run_with_cinderx.py")
        cmd = [sys.executable, script]
    else:
        print(f"Unknown benchmark: {name}", file=sys.stderr)
        return False

    print(f"\n{'='*60}")
    print(f"Running: {name}")
    print(f"{'='*60}")

    try:
        result = subprocess.run(cmd, check=False)
        if result.returncode != 0:
            return False
    except FileNotFoundError:
        print(f"  Error: could not execute {cmd[0]}", file=sys.stderr)
        return False

    return True


def check_deps(name):
    """Check if a benchmark's dependencies are available."""
    if name in LIGHTWEIGHT_BENCHMARKS or name in JIT_COMPILATION_BENCHMARKS:
        return True
    elif name == "fastmark":
        try:
            import importlib.util

            return importlib.util.find_spec("pyperformance") is not None
        except ImportError:
            return False
    elif name == "torchrec_pt2":
        try:
            import importlib.util

            return importlib.util.find_spec("torchrec") is not None
        except ImportError:
            return False
    return True


def main():
    args = sys.argv[1:]

    # Parse --iterations N
    iterations = 3
    if "--iterations" in args:
        idx = args.index("--iterations")
        try:
            iterations = int(args[idx + 1])
            args = args[:idx] + args[idx + 2:]
        except (IndexError, ValueError):
            print("Error: --iterations requires an integer argument", file=sys.stderr)
            sys.exit(1)

    if "--list" in args or "-l" in args:
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
            print(f"  {name}  (pip install -r {req})")
        return

    if "--help" in args or "-h" in args:
        print(__doc__)
        print("Options:")
        print("  --list, -l          List available benchmarks")
        print("  --iterations N      Number of iterations for lightweight benchmarks (default: 3)")
        print("  --help, -h          Show this help message")
        print()
        print("Examples:")
        print("  python -m cinderx.benchmarks")
        print("  python -m cinderx.benchmarks binary_trees fannkuch")
        print("  python -m cinderx.benchmarks compile_time")
        return

    benchmarks_to_run = args if args else ALL_BENCHMARK_NAMES

    # Validate names
    for name in benchmarks_to_run:
        if name not in ALL_BENCHMARK_NAMES:
            print(f"Unknown benchmark: {name}", file=sys.stderr)
            print(f"Run 'python -m cinderx.benchmarks --list' to see available benchmarks")
            sys.exit(1)

    passed = 0
    skipped = 0
    failed = 0

    for name in benchmarks_to_run:
        if not check_deps(name):
            req_file = HEAVYWEIGHT_BENCHMARKS.get(name)
            print(f"\nSkipping {name} — missing dependencies.")
            if req_file:
                print(f"  Install with: pip install -r cinderx/benchmarks/{req_file}")
            skipped += 1
            continue

        success = run_benchmark(name, iterations)
        if success:
            passed += 1
        else:
            failed += 1

    print(f"\n{'='*60}")
    print(f"Results: {passed} passed, {skipped} skipped, {failed} failed")
    print(f"{'='*60}")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
