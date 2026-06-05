# CinderX Benchmarks

Benchmarks for measuring CinderX JIT performance on real-world Python workloads.

## Quick Start

```bash
uv venv
uv pip install setuptools
uv pip install -e . --no-build-isolation --reinstall
uv run python benchmarks/runner.py
uv run python benchmarks/runner.py --iterations 10
```

This runs all benchmarks whose dependencies are available, skipping the rest with
a helpful message about what to install. Lightweight benchmarks run with 3 iterations
by default — use `--iterations N` for more.

## Lightweight Benchmarks

These benchmarks have no extra dependencies beyond cinderx itself:

```bash
uv run python benchmarks/binary_trees.py 5
uv run python benchmarks/fannkuch.py 3
uv run python benchmarks/nbody.py 3
uv run python benchmarks/richards.py 3
uv run python benchmarks/spectral_norm.py 3
```

The numeric argument controls the number of iterations (higher = longer run).

## JIT Compilation Time Benchmark

Measures how long the JIT takes to compile functions (not runtime performance):

```bash
uv run python -m cinderx.benchmarks.compile_time
```

## Heavyweight Benchmarks

These require additional dependencies to be installed.

### Full Suite (fastmark)

The `fastmark` benchmark runs the full pyperformance suite with CinderX:

```bash
uv pip install -r benchmarks/requirements-fastmark.txt
uv run python benchmarks/fastmark.py --cinderx
```

Options:
- `--scale N` — work scale factor (default 100, lower = faster)
- `--json output.json` — save results as JSON
- `--cinderx` — enable the CinderX JIT
- `benchmarks...` — run only specific benchmarks (e.g. `richards chaos`)

### TorchBench benchmark (torchbench)

Runs a real model from [PyTorch's TorchBench suite](https://github.com/pytorch/benchmark), with and without the CinderX JIT.

Most TorchBench models spend almost all their time in fused C++ kernels, leaving
little for a Python JIT to accelerate. The default model,
`pyhpc_equation_of_state`, is a small CPU workload where Python optimization is expected to matter. Any TorchBench model can be selected via `--model`.

TorchBench is not vendored here and is intended to be installed from a source
checkout. From the repository root, install the base packages, clone TorchBench,
and install the default model's requirements:

```bash
uv venv
uv pip install setuptools
uv pip install -e . --no-build-isolation --reinstall
uv pip install -r cinderx/benchmarks/requirements-torchbench.txt
git clone https://github.com/pytorch/benchmark.git ../pytorch-benchmark
cd ../pytorch-benchmark
../cinderx/.venv/bin/python install.py pyhpc_equation_of_state
cd ../cinderx
```

Run this benchmark with the TorchBench checkout on `PYTHONPATH`:

```bash
export PYTHONPATH="$(pwd)/../pytorch-benchmark:${PYTHONPATH}"

# Baseline (interpreter):
uv run python cinderx/benchmarks/torchbench.py --iterations 30

# With the CinderX JIT:
uv run python cinderx/benchmarks/torchbench.py --cinderx --iterations 30

# One-shot comparison (re-execs itself in two subprocesses, prints the speedup):
uv run python cinderx/benchmarks/torchbench.py --compare

# Through the runner:
uv run python cinderx/benchmarks/runner.py torchbench
```

Options:
- `--cinderx` — enable the CinderX JIT
- `--model NAME` — TorchBench model to run (default `pyhpc_equation_of_state`)
- `--test {eval,train}` — TorchBench test mode (default `eval`)
- `--batch-size N` — model input size; smaller keeps the workload Python-bound (default 1024)
- `--iterations N` — number of timed iterations per run (default 30)
- `--warmup N` — warmup iterations before timing so the JIT compiles the hot path (default 20)
- `--repeat N` — number of timed runs (default 3)
- `--compare` — run baseline vs JIT in subprocesses and print the speedup ratio

The `compiled_funcs` count reports how many functions the JIT compiled during the run.

## Running Without CinderX JIT

To get a baseline comparison without JIT compilation, disable it via environment variable:

```bash
# With JIT (default)
uv run python benchmarks/runner.py

# Without JIT (baseline)
CINDERJIT_DISABLE=1 uv run python benchmarks/runner.py
```

## Benchmark Descriptions

| Benchmark | Description |
|-----------|-------------|
| `binary_trees` | Allocation-heavy workload building and traversing complete binary trees |
| `fannkuch` | Combinatorial puzzle exercising array permutations and reversals |
| `nbody` | N-body gravitational simulation with tight floating-point loops |
| `richards` | Operating system task scheduler simulation (object-oriented workload) |
| `spectral_norm` | Numerical computation of the spectral norm of a matrix |
| `compile_time` | Measures JIT compilation speed (not runtime performance) |
| `fastmark` | Full pyperformance suite (~60 benchmarks) with CinderX integration |
| `torchbench` | Run of a real TorchBench model (default `pyhpc_equation_of_state`), kept Python-bound for the JIT |

## Listing Available Benchmarks

```bash
uv run python benchmarks/runner.py --list
```
