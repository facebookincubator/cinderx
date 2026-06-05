# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Run a TorchBench eager CPU workload with and without the CinderX JIT.

The default model is ``pyhpc_equation_of_state``, which is a small eager CPU
workload where Python op dispatch is expected to matter.  TorchBench is not
vendored here; see ``benchmarks/requirements-torchbench.txt`` for setup.
"""

from __future__ import annotations

import os
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
        "TorchBench itself is not pip-installable: clone\n"
        "  https://github.com/pytorch/benchmark\n"
        "and run `python install.py --models <name>` (or add an existing\n"
        "TorchBench checkout to PYTHONPATH).",
        file=sys.stderr,
    )
    sys.exit(1)


DEFAULT_MODEL: str = "pyhpc_equation_of_state"
DEFAULT_BATCH_SIZE: int = 1024


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
    for _ in range(warmup):
        model.invoke()

    samples_ms: list[float] = []
    for i in range(repeat):
        elapsed = run_iterations(model, iterations)
        mean_ms = elapsed / iterations * 1000
        samples_ms.append(mean_ms)
        print(f"  Run {i + 1}/{repeat}: {mean_ms:.2f} ms/iter", file=sys.stderr)

    return samples_ms


def run_compare(argv: list[str]) -> None:
    """Re-exec this script twice (baseline vs JIT) and print the speedup.

    One subprocess runs with ``CINDERJIT_DISABLE=1`` (interpreter baseline), the
    other with ``--cinderx``.  All other args (``--model``, ``--test``,
    ``--batch-size``, ``--iterations``, ``--warmup``) are forwarded so both runs
    measure the same workload.
    """
    forwarded = [a for a in argv if a not in ("--compare", "--cinderx")]
    script = os.path.abspath(__file__)

    def measure(
        label: str,
        env_extra: dict[str, str],
        extra_args: list[str],
        *,
        clear_jit_disable: bool = False,
    ) -> float:
        env = dict(os.environ)
        env.update(env_extra)
        if clear_jit_disable:
            env.pop("CINDERJIT_DISABLE", None)
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

    baseline_ms = measure(
        "Baseline (CINDERJIT_DISABLE=1)", {"CINDERJIT_DISABLE": "1"}, []
    )
    jit_ms = measure("CinderX JIT", {}, ["--cinderx"], clear_jit_disable=True)

    speedup = baseline_ms / jit_ms if jit_ms else float("nan")
    print(f"\n{'=' * 48}")
    print(f"Baseline (no JIT): {baseline_ms:8.3f} ms/iter")
    print(f"CinderX JIT:       {jit_ms:8.3f} ms/iter")
    print(f"Speedup:           {speedup:8.2f}x  (higher is better)")
    print(f"{'=' * 48}")


@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.option("--cinderx", "enable_cinderx", is_flag=True, help="Enable the CinderX JIT")
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
    default=30,
    show_default=True,
    help="Number of timed iterations per run",
)
@click.option(
    "--warmup",
    type=click.IntRange(min=0),
    default=20,
    show_default=True,
    help="Number of warmup iterations before timing",
)
@click.option(
    "--repeat",
    type=click.IntRange(min=1),
    default=3,
    show_default=True,
    help="Number of timed runs",
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
    compare: bool,
) -> None:
    if compare:
        run_compare(sys.argv[1:])
        return

    print(f"Python {sys.version.split()[0]}  torch {torch.__version__}", file=sys.stderr)
    print("CinderX TorchBench eager CPU benchmark", file=sys.stderr)
    print(
        f"model={model} test={test_mode} batch_size={batch_size} "
        f"warmup={warmup} iterations={iterations} repeat={repeat}",
        file=sys.stderr,
    )

    if enable_cinderx:
        print("Enabling the CinderX JIT", file=sys.stderr)
        cinderx.jit.auto()
        cinderx.jit.compile_after_n_calls(1)
        compiled_before = len(cinderx.jit.get_compiled_functions())
        if compiled_before:
            raise RuntimeError(
                "Expected no compiled functions before benchmark setup; "
                f"found {compiled_before}"
            )

    # Single-threaded keeps the workload Python-bound and reduces noise.
    torch.set_num_threads(1)

    print("Setting up model...", file=sys.stderr)
    torchbench_model = build_model(model, test_mode, batch_size)

    print(f"Warmup ({warmup} iterations)...", file=sys.stderr)
    print(f"Timed runs ({repeat} x {iterations} iterations)...", file=sys.stderr)
    samples_ms = run(torchbench_model, iterations, warmup, repeat)

    compiled_after = len(cinderx.jit.get_compiled_functions())
    if enable_cinderx:
        cinderx.jit.disable()

    mean_ms = sum(samples_ms) / len(samples_ms)
    sorted_samples = sorted(samples_ms)
    median_ms = sorted_samples[len(sorted_samples) // 2]

    print("", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    print("CinderX TorchBench results", file=sys.stderr)
    print(f"  model={model} test={test_mode} batch_size={batch_size}", file=sys.stderr)
    print(f"  warmup={warmup} iterations_per_run={iterations} runs={repeat}", file=sys.stderr)
    print(f"  run times: {[f'{sample:.2f}ms' for sample in samples_ms]}", file=sys.stderr)
    print(f"  mean: {mean_ms:.2f} ms/iter", file=sys.stderr)
    print(f"  median: {median_ms:.2f} ms/iter", file=sys.stderr)
    print(
        f"  JIT requested={'yes' if enable_cinderx else 'no'} "
        f"compiled_funcs={compiled_after}",
        file=sys.stderr,
    )
    print("=" * 60, file=sys.stderr)



if __name__ == "__main__":
    cli()
