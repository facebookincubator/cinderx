# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Run a TorchBench eager CPU workload with and without the CinderX JIT.

The default model is ``pyhpc_equation_of_state``, a small eager CPU workload
chosen because Python op dispatch is about as visible here as it gets in
TorchBench.  TorchBench is not vendored here; see
``benchmarks/requirements-torchbench.txt`` for setup.

What this measures, and its limits: eager PyTorch on CPU is *dispatch-bound*,
not interpreter-bound.  Profiling shows ~95% of the time goes to ATen C++ per-op
overhead -- GIL / thread-state handoff, ``TensorIterator`` setup, and tensor
allocation/refcounting -- which no Python JIT can touch; only the few percent
spent interpreting the dispatch bytecode is addressable.  Measured CinderX
speedups are therefore small (~1.0x), even on free-threaded 3.14t where the GIL
cost disappears.  This still makes torchbench a useful, realistic guardrail --
it confirms the JIT stays neutral-to-positive on real eager PyTorch and does not
regress it -- but it is a poor place to look for large wins.
"""

from __future__ import annotations

import contextlib
import os
import statistics
import subprocess
import sys
import time
from typing import Any, Callable

import cinderx.jit
import click


try:
    import torch
    from torchbenchmark import load_model_by_name
    from torchbenchmark.util.extra_args import is_staged_train_test
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

# Per-model default batch sizes set by this benchmark runner.  A model absent from this
# table falls back to its own ``DEFAULT_EVAL_BSIZE``.
#
# ``pyhpc_equation_of_state``'s own default (1048576) is dominated by torch C++ kernels
# and shows no meaningful CinderX signal, so we override it with a small
# Python-dispatch-bound size.
DEFAULT_BATCH_SIZES: dict[str, int] = {
    "pyhpc_equation_of_state": 1024,
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


def resolve_step(model: Any) -> tuple[Callable[[], Any], list[Any]]:
    """Return ``(step, contexts)`` where ``step()`` runs one model iteration.

    The model's per-call ``run_contexts`` (grad mode + JIT profiling executor) are
    hoisted out of the timed loop and the model's ``eval``/``train`` step is called
    directly.  ``BenchmarkModel.invoke`` otherwise rebuilds an ``ExitStack`` plus
    fresh context-manager and closure objects on every call; that churn adds
    eager-Python overhead and keeps CinderX recompiling/cleaning up short-lived
    functions in steady state, neither of which reflects a real serving loop that
    wraps grad mode once around many requests.

    Staged train manages its own per-stage contexts, so it falls back to ``invoke()``
    with no externally-held contexts.
    """
    is_train = model.test == "train"
    if (
        is_train
        and is_staged_train_test(model)
        and getattr(model, "train", None) is None
    ):
        return model.invoke, []
    # Staged train is the only path that loops over ``num_batch``; for every other
    # path ``invoke()`` asserts a single batch per call.  Calling ``eval``/``train``
    # directly once per timed iteration would silently undercount ``num_batch > 1``
    # configs, so keep invoke()'s fail-fast guard.
    assert model.num_batch == 1, (
        "Only staged_train_test supports multiple-batch testing at this time."
    )
    step = model.train if is_train else model.eval
    return step, list(getattr(model, "run_contexts", []))


def run_iterations(step: Callable[[], Any], iterations: int) -> float:
    """Run one timed sample and return elapsed seconds."""
    start = time.perf_counter()
    for _ in range(iterations):
        step()
    return time.perf_counter() - start


def run(model: Any, iterations: int, warmup: int, repeat: int) -> list[float]:
    """Warm up, then collect repeated per-iteration timing samples.

    The model's run-contexts are entered once around the whole measurement rather
    than per iteration (see ``resolve_step``).
    """
    step, contexts = resolve_step(model)
    with contextlib.ExitStack() as stack:
        for make_context in contexts:
            stack.enter_context(make_context())

        print(f"Warmup ({warmup} iterations)...", file=sys.stderr)
        for _ in range(warmup):
            step()

        print(f"Timed runs ({repeat} x {iterations} iterations)...", file=sys.stderr)
        samples_ms: list[float] = []
        for i in range(repeat):
            elapsed = run_iterations(step, iterations)
            mean_ms = elapsed / iterations * 1000
            samples_ms.append(mean_ms)
            print(f"  Run {i + 1}/{repeat}: {mean_ms:.2f} ms/iter", file=sys.stderr)

    return samples_ms


def build_subprocess_env(extra: dict[str, str]) -> dict[str, str]:
    """Build a controlled environment for benchmark subprocesses."""
    env = {key: os.environ[key] for key in SUBPROCESS_ENV_KEYS if key in os.environ}
    env.update(extra)
    return env


def reexec_prefix() -> list[str]:
    """Command prefix that re-runs this benchmark in a fresh process.

    When packaged (a PAR/XAR from ``buck run``/``buck build``) ``__file__`` lives
    under an extracted mount and only the binary itself has the bundled deps on
    ``sys.path``, so re-exec the binary.  A plain ``python script.py`` invocation
    re-execs the interpreter with the script.
    """
    if "/xarfuse/" in os.path.abspath(__file__) or sys.argv[0].endswith(
        (".par", ".xar")
    ):
        return [sys.argv[0]]
    return [sys.executable, os.path.abspath(__file__)]


def run_compare(argv: list[str]) -> None:
    """Re-exec this benchmark twice (baseline vs JIT) and print the speedup.

    One subprocess runs with ``CINDERX_DISABLE=1`` (interpreter baseline), the
    other with ``--cinderx``.  All other args (``--model``, ``--test``,
    ``--batch-size``, ``--iterations``, ``--warmup``) are forwarded so both runs
    measure the same workload.
    """
    forwarded = [a for a in argv if a not in ("--compare", "--cinderx")]
    prefix = reexec_prefix()

    def measure(label: str, env_extra: dict[str, str], extra_args: list[str]) -> float:
        env = build_subprocess_env(env_extra)
        cmd = [*prefix, *forwarded, *extra_args]
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
    batch_size: int | None,
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
    type=click.IntRange(min=1),
    default=None,
    help=(
        "Model input/batch size; smaller keeps the workload Python-bound. "
        "If omitted, uses the per-model default (see DEFAULT_BATCH_SIZES), "
        "falling back to the model's own default batch size."
    ),
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
    batch_size: int | None,
    iterations: int,
    warmup: int,
    repeat: int,
    compile_after_n_calls: int | None,
    compare: bool,
) -> None:
    if compare:
        run_compare(sys.argv[1:])
        return

    # An explicit --batch-size wins; otherwise fall back to the per-model
    # default, then to the model's own default (None).
    if batch_size is None:
        batch_size = DEFAULT_BATCH_SIZES.get(model)

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
