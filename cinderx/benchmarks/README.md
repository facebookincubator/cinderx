# CinderX Benchmarks

Benchmarks for measuring CinderX JIT performance on real-world Python workloads.

## Quick Start

```bash
pip install cinderx
python -m cinderx.benchmarks
python -m cinderx.benchmarks --iterations 10
```

This runs all benchmarks whose dependencies are available, skipping the rest with
a helpful message about what to install. Lightweight benchmarks run with 3 iterations
by default — use `--iterations N` for more:

## Lightweight Benchmarks

These benchmarks have no extra dependencies beyond cinderx itself:

```bash
python -m cinderx.benchmarks.binary_trees 5
python -m cinderx.benchmarks.fannkuch 3
python -m cinderx.benchmarks.nbody 3
python -m cinderx.benchmarks.richards 3
python -m cinderx.benchmarks.spectral_norm 3
```

The numeric argument controls the number of iterations (higher = longer run).

## JIT Compilation Time Benchmark

Measures how long the JIT takes to compile functions (not runtime performance):

```bash
python -m cinderx.benchmarks.compile_time
```

For a per-phase breakdown, pass the `jit-time` flag:

```bash
python -X jit-time='*' -m cinderx.benchmarks.compile_time
```

## Heavyweight Benchmarks

These require additional dependencies to be installed.

### Full Suite (fastmark)

The `fastmark` benchmark runs the full pyperformance suite with CinderX:

```bash
pip install -r cinderx/benchmarks/requirements-fastmark.txt
python -m cinderx.benchmarks.fastmark --cinderx
```

Options:
- `--scale N` — work scale factor (default 100, lower = faster)
- `--json output.json` — save results as JSON
- `--cinderx` — enable the CinderX JIT
- `benchmarks...` — run only specific benchmarks (e.g. `richards chaos`)

### TorchRec Benchmarks

PT2 compilation benchmarks for TorchRec models:

```bash
pip install -r cinderx/benchmarks/requirements-torchrec.txt
python cinderx/benchmarks/torchrec_pt2/run_with_cinderx.py
```

See [torchrec_pt2/README.md](torchrec_pt2/README.md) for details.

## Running Without CinderX JIT

To get a baseline comparison without JIT compilation, disable it via environment variable:

```bash
# With JIT (default)
python -m cinderx.benchmarks

# Without JIT (baseline)
CINDERJIT_DISABLE=1 python -m cinderx.benchmarks
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
| `torchrec_pt2` | TorchRec model compilation benchmarks with PT2 |

## Listing Available Benchmarks

```bash
python -m cinderx.benchmarks --list
```
