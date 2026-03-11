# CinderX + TorchRec pt2_compile_benchmark (CPU)

Benchmarks CinderX JIT impact on the TorchRec `pt2_compile_benchmark`
(eager forward+backward of DistributedModelParallel), adapted to run on CPU.
Uses `FakeStore` and a fake process group — no GPU or multi-node setup needed.

## Running the Benchmark

### CinderX JIT benchmark

```bash
# Quick test (5 features, default)
buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx -- --num-features 5 --repeat 1 --iters 10

# Full benchmark (200 features, 50 iters x 3 runs)
buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx -- --num-features 200 --repeat 3 --iters 50
```

### Without JIT (for comparison)

Set `PYTHONJITDISABLE=1` to run the same binary with the JIT forcefully disabled:

```bash
PYTHONJITDISABLE=1 buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx -- \
    --num-features 200 --repeat 3 --iters 50
```

### Baseline (original compile benchmark, no CinderX)

```bash
buck run //cinderx/benchmarks/torchrec_pt2:baseline -- --num-features 200
```

### Specific Python version

The `cinderx_benchmark_binary` macro generates versioned targets:

```bash
buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx-314 -- --num-features 200
buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx-312 -- --num-features 200
```

## Files

| File | Description |
|---|---|
| `run_with_cinderx.py` | CinderX JIT benchmark runner |
| `test_pt2_multiprocess.py` | Adapted benchmark (CPU, baseline model/sharders) |
| `BUCK` | Build targets |
| `README.md` | This file |

## CPU Adaptations

The original benchmark (`fbcode/torchrec/distributed/tests/test_pt2_multiprocess.py`)
runs on GPU. This version is adapted to run on CPU so it can run anywhere without
GPU or multi-node setup:

1. `torch.device("cuda")` -> `torch.device("cpu")`
2. `compute_device="cuda"` -> `compute_device="cpu"` in Topology
3. `fullgraph=True` -> `fullgraph=False` (fbgemm CPU ops don't support full graph)
4. `convert_to_vb=True` -> `convert_to_vb=False` (VBE path uses unsupported CPU ops)
5. Relaxed eager-vs-compile assertion (CPU fake PG produces NaN in compile path)
