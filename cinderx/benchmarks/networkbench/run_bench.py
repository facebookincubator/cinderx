#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import os
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


DEFAULT_REQUEST_COUNT = 10000
DEFAULT_RUNS = 5
SCRIPT_DIR = Path(__file__).resolve().parent
RUNNER = "run_server_client.py"
JITLIST = "networkbench.jitlist.txt"
REQUESTS_PER_SECOND_RE = re.compile(
    r"Average requests per second:\s+([0-9]+(?:\.[0-9]+)?)"
)


@dataclass(frozen=True)
class BenchmarkCommand:
    name: str
    env: dict[str, str]


@dataclass(frozen=True)
class BenchmarkResult:
    name: str
    requests_per_second: list[float]

    @property
    def mean(self) -> float:
        return statistics.fmean(self.requests_per_second)

    @property
    def min(self) -> float:
        return min(self.requests_per_second)

    @property
    def stdev(self) -> float:
        if len(self.requests_per_second) < 2:
            return 0.0
        return statistics.stdev(self.requests_per_second)

    @property
    def sorted_requests_per_second(self) -> list[float]:
        return sorted(self.requests_per_second, reverse=True)


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare networkbench with a JIT list against CinderX disabled."
    )
    parser.add_argument(
        "request_count",
        nargs="?",
        default=DEFAULT_REQUEST_COUNT,
        type=positive_int,
        help=f"number of client requests per run (default: {DEFAULT_REQUEST_COUNT})",
    )
    parser.add_argument(
        "-n",
        "--runs",
        default=DEFAULT_RUNS,
        type=positive_int,
        help=f"number of runs for each command (default: {DEFAULT_RUNS})",
    )
    parser.add_argument(
        "--python",
        default="python",
        help="Python executable to use for benchmark commands (default: python)",
    )
    return parser.parse_args()


def make_commands(networkbench_dir: Path) -> list[BenchmarkCommand]:
    jitlist = (networkbench_dir / JITLIST).resolve()
    if not jitlist.is_file():
        raise SystemExit(f"JIT list not found: {jitlist}")

    return [
        BenchmarkCommand("cinderx_jitlist", {"PYTHONJITLISTFILE": str(jitlist)}),
        BenchmarkCommand("cinderx_disable", {"CINDERX_DISABLE": "1"}),
    ]


def parse_requests_per_second(output: str) -> float | None:
    match = REQUESTS_PER_SECOND_RE.search(output)
    if match is None:
        return None
    return float(match.group(1))


def run_command(
    command: BenchmarkCommand,
    python: str,
    request_count: int,
    networkbench_dir: Path,
) -> float:
    env = dict(os.environ)
    env.update(command.env)
    args = [python, RUNNER, str(request_count)]

    proc = subprocess.run(
        args,
        cwd=networkbench_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise SystemExit(
            f"{command.name} failed with exit code {proc.returncode}: "
            f"{format_command(command, python, request_count)}"
        )

    requests_per_second = parse_requests_per_second(proc.stdout)
    if requests_per_second is None:
        sys.stderr.write(proc.stdout)
        raise SystemExit(
            f"{command.name} output did not report requests per second: "
            f"{format_command(command, python, request_count)}"
        )

    return requests_per_second


def run_benchmark(
    command: BenchmarkCommand,
    python: str,
    request_count: int,
    runs: int,
    networkbench_dir: Path,
) -> BenchmarkResult:
    requests_per_second = []
    for run in range(1, runs + 1):
        print(f"{command.name}: run {run}/{runs} ...", flush=True)
        requests_per_second.append(
            run_command(command, python, request_count, networkbench_dir)
        )
    return BenchmarkResult(command.name, requests_per_second)


def format_command(command: BenchmarkCommand, python: str, request_count: int) -> str:
    env = " ".join(f'{key}="{value}"' for key, value in command.env.items())
    return f"{env} {python} {RUNNER} {request_count}"


def print_summary(
    results: list[BenchmarkResult],
    commands: list[BenchmarkCommand],
    python: str,
    request_count: int,
) -> None:
    print()
    print("Commands:")
    for command in commands:
        print(f"  {command.name}: {format_command(command, python, request_count)}")

    print()
    print(
        f"{'benchmark':<18} "
        f"{'req/s mean':>12} "
        f"{'req/s min':>12} "
        f"{'req/s stdev':>12}  "
        f"runs"
    )
    print("-" * 72)
    for result in results:
        runs = ", ".join(
            f"{requests_per_second:.2f}"
            for requests_per_second in result.sorted_requests_per_second
        )
        print(
            f"{result.name:<18} "
            f"{result.mean:>12.2f} "
            f"{result.min:>12.2f} "
            f"{result.stdev:>12.2f}  "
            f"{runs}"
        )

    if len(results) == 2:
        baseline, contender = results
        ratio = contender.mean / baseline.mean
        faster = contender.name if ratio >= 1 else baseline.name
        speedup = ratio if ratio >= 1 else 1 / ratio
        print()
        print(f"{faster} reports {speedup:.2f}x higher mean requests/second.")


def main() -> None:
    args = parse_args()
    with contextlib.chdir(SCRIPT_DIR):
        networkbench_dir = Path.cwd().resolve()
        commands = make_commands(networkbench_dir)
        results = [
            run_benchmark(
                command,
                args.python,
                args.request_count,
                args.runs,
                networkbench_dir,
            )
            for command in commands
        ]
        print_summary(results, commands, args.python, args.request_count)


if __name__ == "__main__":
    main()
