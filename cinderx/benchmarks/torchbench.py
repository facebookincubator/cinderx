# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Run a TorchBench eager CPU workload with and without the CinderX JIT.

The default model is ``pyhpc_equation_of_state``, which is a small eager CPU
workload where Python op dispatch is expected to matter.  TorchBench is not
vendored here; see ``benchmarks/requirements-torchbench.txt`` for setup.
"""

from __future__ import annotations

import os
import statistics
import subprocess
import sys
import time
from typing import Any

import cinderx.jit
import click


try:
    import torch
    from torchbenchmark import load_model_by_name
except ImportError:
    print(
        "Error: torch / torchbenchmark not found. Install torch with:\n"
        "  pip install -r benchmarks/requirements-torchbench.txt\n"
        "Then clone https://github.com/pytorch/benchmark, run\n"
        "  python install.py pyhpc_equation_of_state\n"
        "from that checkout, and add the checkout to PYTHONPATH.",
        file=sys.stderr,
    )
    sys.exit(1)


DEFAULT_MODEL: str = "pyhpc_equation_of_state"
DEFAULT_BATCH_SIZE: int = 1024
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


def build_model(name: str, test: str, batch_size: int | None) -> Any:
    """Load and instantiate a TorchBench model in eager mode on CPU."""
    model_cls = load_model_by_name(name)
    if model_cls is None:
        print(f"Error: TorchBench model {name!r} not found.", file=sys.stderr)
        sys.exit(1)
    return model_cls(
        test=test,
        device="cpu",
        batch_size=batch_size,
        extra_args=[],
    )


def run_iterations(model: Any, iterations: int) -> float:
    """Run one timed sample and return elapsed seconds."""
    start = time.perf_counter()
    for _ in range(iterations):
        model.invoke()
    return time.perf_counter() - start


def run(model: Any, iterations: int, warmup: int, repeat: int) -> list[float]:
    """Warm up once, then collect repeated per-iteration timing samples."""
    print(f"Warmup ({warmup} iterations)...", file=sys.stderr)
    for _ in range(warmup):
        model.invoke()

    print(f"Timed runs ({repeat} x {iterations} iterations)...", file=sys.stderr)
    samples_ms: list[float] = []
    for i in range(repeat):
        elapsed = run_iterations(model, iterations)
        mean_ms = elapsed / iterations * 1000
        samples_ms.append(mean_ms)
        print(f"  Run {i + 1}/{repeat}: {mean_ms:.2f} ms/iter", file=sys.stderr)

    return samples_ms


def build_subprocess_env(extra: dict[str, str]) -> dict[str, str]:
    """Build a controlled environment for benchmark subprocesses."""
    env = {key: os.environ[key] for key in SUBPROCESS_ENV_KEYS if key in os.environ}
    env.update(extra)
    return env


def run_compare(argv: list[str]) -> None:
    """Re-exec this script twice (baseline vs JIT) and print the speedup.

    One subprocess runs with ``CINDERX_DISABLE=1`` (interpreter baseline), the
    other with ``--cinderx``.  All other args (``--model``, ``--test``,
    ``--batch-size``, ``--iterations``, ``--warmup``) are forwarded so both runs
    measure the same workload.
    """
    forwarded = [a for a in argv if a not in ("--compare", "--cinderx")]
    script = os.path.abspath(__file__)

    def measure(label: str, env_extra: dict[str, str], extra_args: list[str]) -> float:
        env = build_subprocess_env(env_extra)
        cmd = [sys.executable, script, *forwarded, *extra_args]
        print(f"\n--- {label} ---")
        result = subprocess.run(cmd, env=env, capture_output=True, text=True)
        sys.stdout.write(result.stderr)
        sys.stdout.write(result.stdout)
        if result.returncode:
            print(f"Error: {label} run failed", file=sys.stderr)
            sys.exit(result.returncode)

        mean_ms = None
        for line in result.stderr.splitlines():
            line = line.strip()
            if line.startswith("mean:"):
                mean_ms = float(line.split()[1])
                break
        if mean_ms is None:
            print(f"Error: could not parse mean from {label} run", file=sys.stderr)
            sys.exit(1)
        return mean_ms

    baseline_ms = measure("Baseline (CINDERX_DISABLE=1)", {"CINDERX_DISABLE": "1"}, [])
    jit_ms = measure("CinderX JIT", {}, ["--cinderx"])

    speedup = baseline_ms / jit_ms if jit_ms else float("nan")
    print(f"\n{'=' * 48}")
    print(f"Baseline (no JIT): {baseline_ms:8.3f} ms/iter")
    print(f"CinderX JIT:       {jit_ms:8.3f} ms/iter")
    print(f"Speedup:           {speedup:8.2f}x  (higher is better)")
    print(f"{'=' * 48}")


def print_results(
    model: str,
    test_mode: str,
    batch_size: int,
    warmup: int,
    iterations: int,
    repeat: int,
    enable_cinderx: bool,
    samples_ms: list[float],
) -> None:
    mean_ms = sum(samples_ms) / len(samples_ms)
    median_ms = statistics.median(samples_ms)

    print("", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    print("CinderX TorchBench results", file=sys.stderr)
    print(f"  model={model} test={test_mode} batch_size={batch_size}", file=sys.stderr)
    print(
        f"  warmup={warmup} iterations_per_run={iterations} runs={repeat}",
        file=sys.stderr,
    )
    print(
        f"  run times: {[f'{sample:.2f}ms' for sample in samples_ms]}", file=sys.stderr
    )
    print(f"  mean: {mean_ms:.2f} ms/iter", file=sys.stderr)
    print(f"  median: {median_ms:.2f} ms/iter", file=sys.stderr)
    print(f"  JIT requested={'yes' if enable_cinderx else 'no'}", file=sys.stderr)
    print("=" * 60, file=sys.stderr)


@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.option(
    "--cinderx", "enable_cinderx", is_flag=True, help="Enable the CinderX JIT"
)
@click.option(
    "--model",
    default=DEFAULT_MODEL,
    show_default=True,
    help="TorchBench model name",
)
@click.option(
    "--test",
    "test_mode",
    type=click.Choice(["eval", "train"]),
    default="eval",
    show_default=True,
    help="TorchBench test mode",
)
@click.option(
    "--batch-size",
    type=int,
    default=DEFAULT_BATCH_SIZE,
    show_default=True,
    help="Model input/batch size; smaller keeps the workload Python-bound",
)
@click.option(
    "--iterations",
    type=click.IntRange(min=1),
    default=2000,
    show_default=True,
    help="Number of timed iterations per run",
)
@click.option(
    "--warmup",
    type=click.IntRange(min=0),
    default=1500,
    show_default=True,
    help="Number of warmup iterations before timing",
)
@click.option(
    "--repeat",
    type=click.IntRange(min=1),
    default=5,
    show_default=True,
    help="Number of timed runs",
)
@click.option(
    "--compile-after-n-calls",
    type=click.IntRange(min=0),
    default=None,
    help="Override the JIT call-count threshold when --cinderx is enabled",
)
@click.option(
    "--compare",
    is_flag=True,
    help="Re-exec in two subprocesses (baseline vs JIT) and print the speedup",
)
def cli(
    enable_cinderx: bool,
    model: str,
    test_mode: str,
    batch_size: int,
    iterations: int,
    warmup: int,
    repeat: int,
    compile_after_n_calls: int | None,
    compare: bool,
) -> None:
    if compare:
        run_compare(sys.argv[1:])
        return

    print(
        f"Python {sys.version.split()[0]}  torch {torch.__version__}", file=sys.stderr
    )
    print("CinderX TorchBench eager CPU benchmark", file=sys.stderr)
    print(
        f"model={model} test={test_mode} batch_size={batch_size} "
        f"warmup={warmup} iterations={iterations} repeat={repeat}",
        file=sys.stderr,
    )

    if enable_cinderx:
        print("Enabling the CinderX JIT", file=sys.stderr)
        cinderx.jit.auto()
        if compile_after_n_calls is not None:
            cinderx.jit.compile_after_n_calls(compile_after_n_calls)

    print("Setting up model...", file=sys.stderr)
    torchbench_model = build_model(model, test_mode, batch_size)

    samples_ms = run(torchbench_model, iterations, warmup, repeat)
    print_results(
        model,
        test_mode,
        batch_size,
        warmup,
        iterations,
        repeat,
        enable_cinderx,
        samples_ms,
    )


if __name__ == "__main__":
    cli()
